/*=========================================================================

 Program: FEMUS
 Module: NonLinearImplicitSystem
 Authors: Simone Bnà

 Copyright (c) FEMTTU
 All rights reserved.

 This software is distributed WITHOUT ANY WARRANTY; without even
 the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "NonLinearImplicitSystem.hpp"
#include "LinearEquationSolver.hpp"
#include "NumericVector.hpp"
#include "iomanip"

#include "PetscMatrix.hpp"

namespace femus {



// ------------------------------------------------------------
// NonLinearImplicitSystem implementation
NonLinearImplicitSystem::NonLinearImplicitSystem (MultiLevelProblem& ml_probl,
				const std::string& name_in,
				const unsigned int number_in, const MgSmoother & smoother_type) :
  LinearImplicitSystem (ml_probl, name_in, number_in, smoother_type),
  _n_nonlinear_iterations   (0),
  _n_max_nonlinear_iterations (15),
  _final_nonlinear_residual (1.e20),
  _max_nonlinear_convergence_tolerance(1.e-6)
{
}

NonLinearImplicitSystem::~NonLinearImplicitSystem() {
   this->clear();
}

void NonLinearImplicitSystem::clear() {
}

void NonLinearImplicitSystem::init() {
  Parent::init();
}

void NonLinearImplicitSystem::solve() {

  clock_t start_mg_time = clock();

  bool full_cycle;
  unsigned igrid0;

  if(_mg_type == F_CYCLE) {
    full_cycle=1;
    igrid0=1;
  }
  else if(_mg_type == V_CYCLE){
    full_cycle=0;
    igrid0=_gridn;
  }
  else {
    full_cycle=0;
    igrid0=_gridr;
  }

  unsigned AMR_counter=0;

  for ( unsigned igridn=igrid0; igridn <= _gridn; igridn++) {   //_igridn

    std::cout << std::endl << "    ************* Level Max: " << igridn << " *************\n" << std::endl;

    bool ThisIsAMR = (_mg_type == F_CYCLE && _AMRtest &&  AMR_counter<_maxAMRlevels && igridn==_gridn)?1:0;
    if(ThisIsAMR) _solution[igridn-1]->InitAMREps();


    for ( _n_nonlinear_iterations = 0; _n_nonlinear_iterations < _n_max_nonlinear_iterations; _n_nonlinear_iterations++ ) { //non linear cycle
      clock_t start_time = clock();
      std::cout << std::endl << "    ************** NonLinear Cycle: " << _n_nonlinear_iterations + 1 << " ****************\n" << std::endl;
      // ============== Fine level Assembly ==============
      _LinSolver[igridn-1u]->SetResZero();
      _LinSolver[igridn-1u]->SetEpsZero();
      _assembleMatrix = true; //Be carefull!!!! this is needed in the _assemble_function
      _levelToAssemble = igridn-1u;
      _levelMax = igridn-1u;
      _assemble_system_function( _equation_systems );

#ifndef NDEBUG
      std::cout << "Grid: " << igridn-1 << "\t        ASSEMBLY TIME:\t"<<static_cast<double>((clock()-start_time))/CLOCKS_PER_SEC << std::endl;
#endif
      for(_n_linear_iterations = 0; _n_linear_iterations < _n_max_linear_iterations; _n_linear_iterations++) { //linear cycle

	bool ksp_clean=!_n_linear_iterations;

	for (unsigned ig = igridn-1u; ig > 0; ig--) {


	  // ============== Presmoothing ==============
	  for (unsigned k = 0; k < _npre; k++) {
	    _LinSolver[ig]->solve(_VariablesToBeSolvedIndex , ksp_clean*(!k));
	  }
	  // ============== Non-Standard Multigrid Restriction ==============
	  start_time = clock();
	  Restrictor(ig, igridn, _n_nonlinear_iterations, _n_linear_iterations, full_cycle);
#ifndef NDEBUG
	  std::cout << "Grid: " << ig << "-->" << ig-1 << "   RESTRICTION TIME:\t          "<<static_cast<double>((clock()-start_time))/CLOCKS_PER_SEC << std::endl;
#endif
	}
        // ============== Coarse Direct Solver ==============
        _LinSolver[0]->solve(_VariablesToBeSolvedIndex, ksp_clean);

 	for (unsigned ig = 1; ig < igridn; ig++) {

 	  // ============== Standard Prolongation ==============
 	  start_time=clock();
 	  Prolongator(ig);
#ifndef NDEBUG
 	  std::cout << "Grid: " << ig-1 << "-->" << ig << "  PROLUNGATION TIME:\t          " << static_cast<double>((clock()-start_time))/CLOCKS_PER_SEC << std::endl;
#endif
 	  // ============== PostSmoothing ==============
          for (unsigned k = 0; k < _npost; k++) {
	    _LinSolver[ig]->solve(_VariablesToBeSolvedIndex, ksp_clean*(!_npre)*(!k));
	  }
 	}
 	// ============== Update Solution ( _gridr-1 <= ig <= igridn-2 ) ==============
 	for (unsigned ig = _gridr-1; ig < igridn-1; ig++) {  // _gridr
 	  _solution[ig]->SumEpsToSol(_SolSystemPdeIndex, _LinSolver[ig]->_EPS, _LinSolver[ig]->_RES, _LinSolver[ig]->KKoffset );
 	}

	_solution[igridn-1]->UpdateRes(_SolSystemPdeIndex, _LinSolver[igridn-1]->_RES, _LinSolver[igridn-1]->KKoffset );
	bool islinearconverged = IsLinearConverged(igridn-1);
// 	if(_final_linear_residual < _absolute_convergence_tolerance)
	if(islinearconverged)
	  break;

      }

      // ============== Update Solution ( ig = igridn )==============
      _solution[igridn-1]->SumEpsToSol(_SolSystemPdeIndex, _LinSolver[igridn-1]->_EPS,
				       _LinSolver[igridn-1]->_RES, _LinSolver[igridn-1]->KKoffset );
      // ============== Test for non-linear Convergence ==============
      bool isnonlinearconverged = IsNonLinearConverged(igridn-1);
      if (isnonlinearconverged)
	_n_nonlinear_iterations = _n_max_nonlinear_iterations+1;

#ifndef NDEBUG
      std::cout << std::endl;
      std::cout << "COMPUTATION RESIDUAL: \t"<<static_cast<double>((clock()-start_time))/CLOCKS_PER_SEC << std::endl;
#endif
    }

    if(ThisIsAMR){
      bool conv_test=0;
      if(_AMRnorm==0){
	conv_test=_solution[_gridn-1]->FlagAMRRegionBasedOnl2(_SolSystemPdeIndex,_AMRthreshold);
      }
      else if (_AMRnorm==1){
	conv_test=_solution[_gridn-1]->FlagAMRRegionBasedOnSemiNorm(_SolSystemPdeIndex,_AMRthreshold);
      }
      if(conv_test==0){
	_ml_msh->AddAMRMeshLevel();
	_ml_sol->AddSolutionLevel();
	AddSystemLevel();
	AMR_counter++;
      }
      else{
	_maxAMRlevels=AMR_counter;
	std::cout<<"The AMR solver has converged after "<<AMR_counter<<" refinements.\n";
      }
    }

    // ==============  Solution Prolongation ==============
    if (igridn < _gridn) {
      ProlongatorSol(igridn);
    }
  }

  std::cout << "SOLVER TIME:   \t\t\t"<< std::setw(11) << std::setprecision(6) << std::fixed <<
  static_cast<double>((clock()-start_mg_time))/CLOCKS_PER_SEC << std::endl;

}



bool NonLinearImplicitSystem::IsNonLinearConverged(const unsigned igridn){

  bool conv=true;
  double ResMax;
  double L2normEps;

  std::cout << std::endl;
  //for debugging purpose
  for (unsigned k=0; k<_SolSystemPdeIndex.size(); k++) {
    unsigned indexSol=_SolSystemPdeIndex[k];

    L2normEps    = _solution[igridn]->_Eps[indexSol]->l2_norm();
    ResMax       = _solution[igridn]->_Res[indexSol]->linfty_norm();

    std::cout << "level=" << igridn<< "\tLinftynormRes" << std::scientific << _ml_sol->GetSolutionName(indexSol) << "=" << ResMax    <<std::endl;
    std::cout << "level=" << igridn<< "\tL2normEps"     << std::scientific << _ml_sol->GetSolutionName(indexSol) << "=" << L2normEps <<std::endl;

    if (L2normEps <_max_nonlinear_convergence_tolerance && conv==true) {
      conv=true;
    }
    else {
      conv=false;
    }
  }
  std::cout << std::endl;
  return conv;
}

//   void NonLinearImplicitSystem::PETSCsolve (){
//
//   clock_t start_mg_time = clock();
//   for ( _n_nonlinear_iterations = 0; _n_nonlinear_iterations < _n_max_nonlinear_iterations; _n_nonlinear_iterations++ ) {
//   KSPCreate(PETSC_COMM_WORLD,&_kspMG);
//   KSPGetPC(_kspMG,&_pcMG);
//   PCSetType(_pcMG,PCMG);
//   PCMGSetLevels(_pcMG,_gridn,NULL);
//
//   if( _mg_type == F_CYCLE ){
//     PCMGSetType(_pcMG, PC_MG_FULL);
//   }
//   else if( _mg_type == MULTIPLICATIVE ){
//     PCMGSetType(_pcMG, PC_MG_MULTIPLICATIVE);
//   }
//   else if( _mg_type == ADDITIVE ){
//     PCMGSetType(_pcMG, PC_MG_ADDITIVE);
//   }
//   else if( _mg_type == KASKADE ){
//     PCMGSetType(_pcMG, PC_MG_KASKADE);
//   }
//   else{
//     std::cout <<"Wrong mg_type for PETSCsolve()"<<std::endl;
//     abort();
//   }
//
//   _LinSolver[_gridn-1u]->SetResZero();
//
//   _assembleMatrix = true;
//   _levelToAssemble = _gridn-1u;
//   _levelMax = _gridn-1u;
//   _assemble_system_function(_equation_systems );
//
//   for( unsigned i =_gridn-1u; i > 0; i-- ){
//     _LinSolver[i-1u]->_KK->matrix_PtAP(*_PP[i],*_LinSolver[i]->_KK, false);
//   }
//
//   for( unsigned i = 0; i < _gridn; i++ ){
//     _LinSolver[i]->SetMGOptions(_pcMG, i, _gridn-1, _VariablesToBeSolvedIndex, true);
//
//     if( i > 0 ){
//       PetscMatrix* PPp=static_cast< PetscMatrix* >(_PP[i]);
//       Mat PP=PPp->mat();
//       PCMGSetInterpolation(_pcMG, i, PP);
//       PCMGSetRestriction(_pcMG, i, PP);
//     }
//   }
//
//   PCMGSetNumberSmoothDown(_pcMG, _npre);
//   PCMGSetNumberSmoothUp(_pcMG, _npost);
//
//   for(_n_linear_iterations = 0; _n_linear_iterations < _n_max_linear_iterations; _n_linear_iterations++) { //linear cycle
//     std::cout << " ************* MG-Cycle : "<< _n_linear_iterations << " *************" << std::endl;
//     bool ksp_clean=!_n_linear_iterations;
//     _LinSolver[_gridn-1u]->SetEpsZero();
//     _LinSolver[_gridn-1u]->MGsolve(_kspMG, ksp_clean);
//     _solution[_gridn-1]->UpdateRes(_SolSystemPdeIndex, _LinSolver[_gridn-1]->_RES, _LinSolver[_gridn-1]->KKoffset );
//     bool islinearconverged = IsLinearConverged(_gridn-1u);
//     if(islinearconverged)
//       break;
//   }
//
//   _solution[_gridn-1u]->SumEpsToSol(_SolSystemPdeIndex, _LinSolver[_gridn-1u]->_EPS,
//                                     _LinSolver[_gridn-1u]->_RES, _LinSolver[_gridn-1u]->KKoffset );
//
//    bool isnonlinearconverged = IsNonLinearConverged(_gridn-1);
//    if (isnonlinearconverged)
//      _n_nonlinear_iterations = _n_max_nonlinear_iterations+1;
//
//   KSPDestroy(&_kspMG);
//
//   }
//   std::cout << "\t     SOLVER TIME:\t       " << std::setw(11) << std::setprecision(6) << std::fixed
//   <<static_cast<double>((clock()-start_mg_time))/CLOCKS_PER_SEC << std::endl;
// }



} //end namespace femus

