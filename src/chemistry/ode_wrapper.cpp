//======================================================================================
// Athena++ astrophysical MHD code
// Copyright (C) 2014 James M. Stone  <jmstone@princeton.edu>
//
// This program is free software: you can redistribute and/or modify it under the terms
// of the GNU General Public License (GPL) as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A 
// PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of GNU GPL in the file LICENSE included in the code
// distribution.  If not see <http://www.gnu.org/licenses/>.
//======================================================================================
//! \file ode_wrapper.cpp
//  \brief implementation of functions in class ODEWrapper. This is a wrapper
//  for the ODE solver, CVODE.
//======================================================================================

//c++ header
#include <stdio.h> //c style io
#include <string>
#include <stdexcept> //throw exceptions

// Athena++ classes headers
#include "../parameter_input.hpp"
#include "network/network.hpp"
#include "../mesh.hpp"

//TODO: lib directory
#include <cvode/cvode.h> // CV_SUCCESS
#include <nvector/nvector_serial.h> // N_Vector
#include <sundials/sundials_dense.h> // definitions DlsMat DENSE_ELEM
#include <sundials/sundials_types.h> // realtype 
#include <cvode/cvode.h>            // CVODE solver fcts., consts
#include <cvode/cvode_dense.h>       // prototype for CVDense 

// this class header
#include "species.hpp"

ODEWrapper::ODEWrapper(ChemSpecies *pspec, ParameterInput *pin) {
  int flag;
  pmy_spec_ = pspec;

  //allocate y_
  y_ = N_VNew_Serial(NSPECIES);
  CheckFlag((void *)y_, "N_VNew_Serial", 0);
  ydata_ = NV_DATA_S(y_);
  
  //tolerance
  reltol_ = pin->GetOrAddReal("chemistry", "reltol", 1.0e-2);
  Real abstol_all = pin->GetOrAddReal("chemistry", "abstol", 1.0e-12);
  for (int i=0; i<NSPECIES; i++) {
    abstol_[i] = pin->GetOrAddReal("chemistry",
        "abstol_"+pmy_spec_->pchemnet->species_names[i], -1);
    if (abstol_[i] < 0) {
      abstol_[i] = abstol_all;
    }
  }
  //user input Jacobian flag
  int user_jac = pin->GetOrAddInteger("chemistry", "user_jac", 0);

  // -----------Initialize absolute value vector----------
  N_Vector abstol_vec = N_VNew_Serial(NSPECIES);
  CheckFlag((void *)abstol_vec, "N_VNew_Serial", 0);
  for (int i=0; i<NSPECIES; i++) {
    NV_Ith_S(abstol_vec, i) = abstol_[i];
  }

  //-------------initialize CVODE------------------
  // Call CVodeCreate to create the solver memory and specify the 
  // Backward Differentiation Formula and the use of a Newton iteration
  cvode_mem_ = CVodeCreate(CV_BDF, CV_NEWTON);
  CheckFlag((void *)cvode_mem_, "CVodeCreate", 0);

  // Set the user data pointer to NetworkWrapper
  flag = CVodeSetUserData(cvode_mem_, pmy_spec_->pchemnet);
  CheckFlag(&flag, "CVodeSetUserData", 1);

  // Call CVodeInit to initialize the integrator memory and specify the
  // user's right hand side function in y'=f(t,y), the inital time T0, and
  // the initial dependent variable vector y.
  flag = CVodeInit(cvode_mem_,  pmy_spec_->pchemnet->WrapRHS, 
                   pmy_spec_->pmy_block->pmy_mesh->time, y_);
  CheckFlag(&flag, "CVodeInit", 1);

  // Call CVodeSVtolerances to specify the scalar relative tolerance
  // and vector absolute tolerances 
  flag = CVodeSVtolerances(cvode_mem_, reltol_, abstol_vec);
  CheckFlag(&flag, "CVodeSVtolerances", 1);

  // Call CVDense to specify the CVDENSE dense linear solver
  flag = CVDense(cvode_mem_, NSPECIES);
  CheckFlag(&flag, "CVDense", 1);

  // Set the Jacobian routine to Jac (user-supplied)
	if (user_jac) {
		flag = CVDlsSetDenseJacFn(cvode_mem_, pmy_spec_->pchemnet->WrapJacobian);
		CheckFlag(&flag, "CVDlsSetDenseJacFn", 1);
	}

  //Free abstol_ vector
  N_VDestroy_Serial(abstol_vec);
  
  
}

ODEWrapper::~ODEWrapper() {
  NV_DATA_S(y_) = ydata_;
  //Free y_ vector
  N_VDestroy_Serial(y_);
  // Free integrator memory
  CVodeFree(&cvode_mem_);
}


void ODEWrapper::Integrate() {
  int is = pmy_spec_->pmy_block->is;
  int js = pmy_spec_->pmy_block->js;
  int ks = pmy_spec_->pmy_block->ks;
  int ie = pmy_spec_->pmy_block->ie;
  int je = pmy_spec_->pmy_block->je;
  int ke = pmy_spec_->pmy_block->ke;
  Real *pdata_s1 = pmy_spec_->s1.GetArrayPointer();
  Real tinit = pmy_spec_->pmy_block->pmy_mesh->time;
  Real dt = pmy_spec_->pmy_block->pmy_mesh->dt;
  Real tfinal = tinit + dt;
  Real treturn = 0;
  int flag;
  for (int k=ks; k<=ke; ++k) {
    for (int j=js; j<=je; ++j) {
      //copy s to s1
      for (int ispec=0; ispec<NSPECIES; ispec++) {
        for (int i=is; i<=ie; ++i) {
          pmy_spec_->s1(i, ispec) = pmy_spec_->s(ispec, k, j, i); 
        }
      }
      //loop over each cell
      for (int i=is; i<=ie; ++i) {
        //step 1: initialize chemistry network
        pmy_spec_->pchemnet->Initialize();
        //step 2: re-initialize CVODE with starting time t, and vector y
        //allocate s1(i, *) to y_.
        //TODO: make sure Real and realtype are the same.
        NV_DATA_S(y_) = pdata_s1 + i*NSPECIES;
        flag = CVodeReInit(cvode_mem_, tinit, y_);
        CheckFlag(&flag, "CVodeReInit", 1);
        //step 3: integration. update array abundance over time dt
        flag = CVode(cvode_mem_, tfinal, y_, &treturn, CV_NORMAL);
        CheckFlag(&flag, "CVode", 3);
      }
      //copy s1 back to s
      for (int ispec=0; ispec<NSPECIES; ispec++) {
        for (int i=is; i<=ie; ++i) {
          pmy_spec_->s(ispec, k, j, i) = pmy_spec_->s1(i, ispec); 
        }
      }
    }
  }
  return;
}

void ODEWrapper::SolveEq() {
  return;
}

void ODEWrapper::CheckFlag(const void *flagvalue, const char *funcname, 
               const int opt) {
   int *errflag;

   // Check if SUNDIALS function returned NULL pointer - no memory allocated 
   if (opt == 0 && flagvalue == NULL) {
     fprintf(stderr, "\nSUNDIALS_ERROR: %s() failed - returned NULL pointer\n\n",
         funcname);
     throw std::runtime_error("SUNDIALS:Sundials error.");
     return; 
   }

   // Check if flag < 0 
   else if (opt == 1) {
     errflag = (int *) flagvalue;
     if (*errflag < 0) {
       fprintf(stderr, "\nSUNDIALS_ERROR: %s() failed with flag = %d\n\n",
           funcname, *errflag);
       throw std::runtime_error("SUNDIALS:Sundials error.");
       return; 
     }
   }

   // Check if function returned NULL pointer - no memory allocated 
   else if (opt == 2 && flagvalue == NULL) {
     fprintf(stderr, "\nMEMORY_ERROR: %s() failed - returned NULL pointer\n\n",
         funcname);
     throw std::runtime_error("SUNDIALS:Memory error.");
     return; 
   }

   // Check if CV_SUCCESS for integration.
   else if (opt == 3) {
     errflag = (int *) flagvalue;
     if (*errflag != CV_SUCCESS) {
       fprintf(stderr, "\nCV_SUCCESS error: %s() failed with flag = %d\n\n",
           funcname, *errflag);
       throw std::runtime_error("SUNDIALS:CV_SUCCESS error");
       return; 
     }
   }
}
