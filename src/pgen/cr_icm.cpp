//======================================================================================
/* Athena++ astrophysical MHD code
 * Copyright (C) 2014 James M. Stone  <jmstone@princeton.edu>
 *
 * This program is free software: you can redistribute and/or modify it under the terms
 * of the GNU General Public License (GPL) as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of GNU GPL in the file LICENSE included in the code
 * distribution.  If not see <http://www.gnu.org/licenses/>.
 *====================================================================================*/

// C++ headers
#include <algorithm>  // min
#include <cmath>      // sqrt
#include <fstream>
#include <iostream>   // endl
#include <sstream>    // stringstream
#include <stdexcept>  // runtime_error
#include <string>     // c_str()

// Athena++ headers
#include "../athena.hpp"
#include "../athena_arrays.hpp"
#include "../bvals/bvals.hpp"
#include "../coordinates/coordinates.hpp"
#include "../cr/cr.hpp"
#include "../cr/integrators/cr_integrators.hpp"
#include "../eos/eos.hpp"
#include "../field/field.hpp"
#include "../globals.hpp"
#include "../hydro/hydro.hpp"
#include "../hydro/srcterms/hydro_srcterms.hpp"
#include "../mesh/mesh.hpp"
#include "../parameter_input.hpp"
#include "../fft/turbulence.hpp"


//======================================================================================
//! \fn void MeshBlock::ProblemGenerator(ParameterInput *pin)
//  \brief cosmic ray modified turbulence in the intracluster medium
//======================================================================================
const double k_B = 1.380648999999999994e-16;               
const double c = 2.9979245800e+10;                  
const double l_scale = 3.085677581491367313408e+21; // kpc
const double t_scale = 3.15576000000e+13; // Myr
const double m_scale = 1.672621923689999956e-24; //m_p
const double n_scale = 1.0e-2; // cm^-3
const double v_scale = l_scale/t_scale;
const double rho_scale = m_scale*n_scale;
const double e_scale = rho_scale*v_scale*v_scale;
const double T_scale = m_scale*v_scale*v_scale/k_B;
const double B_scale = 4*PI*sqrt(e_scale);
const double lamb_scale = e_scale/(t_scale*n_scale*n_scale);

// double totalVolume;

double sigmaParl, sigmaPerp; //CR diffusion 
                           //decouple parallel (to local B field) and perpendicular diffusion coefficients
Real crLoss; //CR Loss term. E.g. Hadronic losses, proportional to local CR energy
             // can be useful to give a decay to CR energy during turbulent driving

int cooling_flag;
int heating_flag;
Real turb_dedt;

static Real f_i, T_f_i, dT_f_i;
static Real decouple ;


// Cooling Function parameters: 
// Tupper   [2.e+03 8.e+03 1.e+05 4.e+07 1.e+10] K
// Tlower   [3.e+02 2.e+03 8.e+03 1.e+05 4.e+07] K
// Lks      [2.2380e-32 1.0012e-30 4.6240e-36 1.7800e-18 3.2217e-27] cm3 erg / s
// alphaks  [ 2.     1.5    2.867 -0.65   0.5  ]
// TEF bounds for Tmax= 10000000000.0  K
// Yks      [1.91682979 1.91275108 1.91203155 1.91184059 1.87350889 0.   
double Tupps[5] = {2.0e+3,8.0e+3,1.0e+5,4.0e+7,1e+10};
double Tlows[5] = {3.0e+2,2.0e+3,8.0e+3,1.0e+5,4.0e+7};
double Lks[5] = {2.2380e-32, 1.0012e-30, 4.6240e-36, 1.7800e-18, 3.2217e-27};
double aks[5] = {2.0,1.5,2.867,-0.65,0.5};
double Yks[6] = {1.9168297910695002, 1.9127510824010463 , 1.9120315518174356,1.911840589669069 , 1.8735088935932651, 0.0};
double Tmax = 1.0e+10;
double LN =  Lks[4] * std::pow(Tmax,aks[4]);

double TEF(double T);
double invTEF(double T);

double TEF(double T){
  double val = 0.0;
  int j = 0;
  double Temp = T*T_scale;
  
  //Figure out which T bin we are in
  while(Temp >= Tupps[j]) j++;

  //Calculate Y function value at Temparture T*T_scale
  val = (1/(1-aks[j]))*(LN/Lks[j])*std::pow(Tlows[j],-1*aks[j])*(Tlows[j]/Tmax);
  val *= (1-std::pow(Tlows[j]/(Temp),aks[j]-1));
  val += Yks[j];
  return val;
}

double invTEF(double y) {
  double val = 0.0;
  int j = 0;
  //Figure out which T bin we are in
  if (y> Yks[0]) {
    val = Tlows[0];
  } else {
    while(y<= Yks[j]) j++;
    j -= 1;
  }
  // Calculate invY
  val = (Lks[j]/LN)*std::pow(Tlows[j],aks[j])*(Tmax/Tlows[j]);
  val *= (y-Yks[j])*(1-aks[j]);
  val = 1-val;
  val = (Tlows[j]/T_scale)*std::pow(val,1.0/(1-aks[j]));
  return val;
}   

void CRSource(MeshBlock *pmb, const Real time, const Real dt,
                const AthenaArray<Real> &prim, FaceField &b, 
              AthenaArray<Real> &u_cr);

void mySource(MeshBlock *pmb, const Real time, const Real dt,
               const AthenaArray<Real> &prim, const AthenaArray<Real> &prim_scalar,
               const AthenaArray<Real> &bcc, AthenaArray<Real> &cons,
               AthenaArray<Real> &cons_scalar);
              

void Opacity(MeshBlock *pmb, AthenaArray<Real> &u_cr,
        AthenaArray<Real> &prim, AthenaArray<Real> &bcc);

void Streaming(MeshBlock *pmb, AthenaArray<Real> &u_cr,
             AthenaArray<Real> &prim, AthenaArray<Real> &bcc,
             AthenaArray<Real> &grad_pc, int k, int j, int is, int ie);

Real TotalHeating(MeshBlock *pmb, int iout);

Real Ec_source(MeshBlock *pmb,int iout);

Real correlation(MeshBlock *pmb,int iout);
Real div_correlation(MeshBlock *pmb,int iout);
// Real Correlation(MeshBlock *pmb, int iout);

Real TotalHeating(MeshBlock *pmb, int iout){
  // Real heat=0;
  int is=pmb->is, ie=pmb->ie, js=pmb->js, je=pmb->je, ks=pmb->ks, ke=pmb->ke;
  Real pfloor = pmb->peos->GetPressureFloor();
  Real dfloor = pmb->peos->GetDensityFloor();
  Real Tfloor = Tlows[0]/T_scale;
  double gm1 = pmb->peos->GetGamma()-1.0;

  double totdE = 0.0;
  double totV = 0.0;
  
  AthenaArray<Real> &cons = pmb->phydro->u;
  AthenaArray<Real> &bcc = pmb->pfield->bcc;
  Real dt = pmb->pmy_mesh->dt;
  for (int k=pmb->ks; k<=pmb->ke; ++k) {
    for (int j=pmb->js; j<=pmb->je; ++j) {
#pragma omp simd
      for (int i=pmb->is; i<=pmb->ie; ++i) {
        double d = cons(IDN,k,j,i);
        double p = gm1*(cons(IEN,k,j,i) - 0.5*(SQR(cons(IM1,k,j,i))+SQR(cons(IM2,k,j,i))+SQR(cons(IM3,k,j,i)))/d - 0.5*(SQR(bcc(IB1,k,j,i))+SQR(bcc(IB2,k,j,i))+SQR(bcc(IB3,k,j,i))));

        if ((d> dfloor) && (p> pfloor) ) {
          double T = p/d;
          if ((T > Tfloor)){
            double time0 = t_scale*gm1*d*n_scale*LN/(k_B*Tmax);
            double newT = invTEF(TEF(T) + dt*time0);
            double dE = d*(newT-T)/gm1;
            totdE += -1*dE * pmb->pcoord->GetCellVolume(k,j,i);
            totV += pmb->pcoord->GetCellVolume(k,j,i);
          }
        }
      }
    }
  }
  Real heat = totdE / totV;
  if (iout == 0) return totdE;
  if (iout == 1) return totV;
  
  return heat;
}

Real Ec_source(MeshBlock *pmb, int iout){
  // Real heat=0;
  int is=pmb->is, ie=pmb->ie, js=pmb->js, je=pmb->je, ks=pmb->ks, ke=pmb->ke;
  // Real pfloor = pmb->peos->GetPressureFloor();
  // Real dfloor = pmb->peos->GetDensityFloor();
  // Real Tfloor = Tlows[0]/T_scale;
  // double gm1 = pmb->peos->GetGamma()-1.0;

  double totdE = 0.0;
  
  AthenaArray<Real> &cons = pmb->phydro->u;
  AthenaArray<Real> &prim = pmb->phydro->w;
  AthenaArray<Real> &bcc = pmb->pfield->bcc;
  AthenaArray<Real> &u_cr = pmb->pcr->u_cr;
  
  Real dt = pmb->pmy_mesh->dt;
  for (int k=pmb->ks; k<=pmb->ke; ++k) {
    for (int j=pmb->js; j<=pmb->je; ++j) {
#pragma omp simd
      for (int i=pmb->is; i<=pmb->ie; ++i) {
        Real dens = cons(IDN,k,j,i);
        Real v_1, v_2, v_3 = 0.0;
        if (iout==2){
          v_1 = cons(IM1,k,j,i)/dens;
          v_2 = cons(IM2,k,j,i)/dens;
          v_3 = cons(IM3,k,j,i)/dens;
        } else if (iout == 3){
          Real Temp = prim(IPR,k,j,i)/prim(IDN,k,j,i);
          Real switch_func = 0.5*(1+std::tanh( (Temp - T_f_i)/dT_f_i));
          Real my_fi = (1-f_i)*switch_func + f_i;
          // Real my_fi = std::pow(10,(1-std::log10(f_i))*switch_func + std::log10(f_i));
          Real inv_sqrt_rho = 1.0/std::sqrt(cons(IDN,k,j,i) * my_fi);
          v_1 = bcc(IB1,k,j,i)*inv_sqrt_rho;
          v_2 = bcc(IB2,k,j,i)*inv_sqrt_rho;
          v_3 = bcc(IB3,k,j,i)*inv_sqrt_rho;
        }

        Real grad_1 = (1.0/3.0)/pmb->pcoord->GetEdge1Length(k,j,i);
        Real grad_2 = (1.0/3.0)/pmb->pcoord->GetEdge2Length(k,j,i);
        Real grad_3 = (1.0/3.0)/pmb->pcoord->GetEdge3Length(k,j,i);
        if (v_1 > 0){
          grad_1 *= u_cr(CRE,k,j,i+1) - u_cr(CRE,k,j,i);
        } else {
          grad_1 *= u_cr(CRE,k,j,i) - u_cr(CRE,k,j,i-1);
        }
        if (v_2 > 0){
          grad_2 *= u_cr(CRE,k,j+1,i) - u_cr(CRE,k,j,i);
        } else {
          grad_2 *= u_cr(CRE,k,j,i) - u_cr(CRE,k,j-1,i);
        }
        if (v_3 > 0){
          grad_3 *= u_cr(CRE,k+1,j,i) - u_cr(CRE,k,j,i);
        } else {
          grad_3 *= u_cr(CRE,k,j,i) - u_cr(CRE,k-1,j,i);
        }
        if (iout==2){
          totdE += grad_1 * v_1 + grad_2 * v_2 + grad_3 * v_3  ;
        } else if (iout == 3){
          totdE += std::abs(grad_1 * v_1) + std::abs(grad_2 * v_2) + std::abs(grad_3 * v_3)  ;
        }
        
      }
    }
  }
  
  return totdE;
}

Real correlation(MeshBlock *pmb, int iout){

  int is=pmb->is, ie=pmb->ie, js=pmb->js, je=pmb->je, ks=pmb->ks, ke=pmb->ke;

  double corr = 0.0;
  double var1 = 0.0;
  double var2 = 0.0;
  double vol_tot = 0.0;
  
  AthenaArray<Real> &cons = pmb->phydro->u;
  AthenaArray<Real> &bcc = pmb->pfield->bcc;
  AthenaArray<Real> &u_cr = pmb->pcr->u_cr;
  Real dt = pmb->pmy_mesh->dt;
  for (int k=pmb->ks; k<=pmb->ke; ++k) {
    for (int j=pmb->js; j<=pmb->je; ++j) {
#pragma omp simd
      for (int i=pmb->is; i<=pmb->ie; ++i) {
        Real vol = pmb->pcoord->GetCellVolume(k,j,i);
        Real dens = cons(IDN,k,j,i);
        Real cr_e = u_cr(CRE,k,j,i);
        var1 += dens * vol;
        var2 += cr_e * vol;
        corr += cr_e*dens*vol;
        vol_tot += vol;
      }
    }
  }
  Real out = 0.0;
  if (iout==4){ 
    out = corr / (var1 * var2) * vol_tot;
  }
  else if(iout==5){
    // using hadronic loss rate from Guo & Oh 2008 of -5.86e-16 erg s^-1 cm^-3 into L_sun
    out = corr* 7192.30777903 ;
  } else if (iout==6){
    out = corr * crLoss;
  }
  return out;
}
Real div_correlation(MeshBlock *pmb, int iout){

  int is=pmb->is, ie=pmb->ie, js=pmb->js, je=pmb->je, ks=pmb->ks, ke=pmb->ke;

  double corr = 0.0;
  double var1 = 0.0;
  double var2 = 0.0;
  double vol_tot = 0.0;
  
  AthenaArray<Real> &cons = pmb->phydro->u;
  AthenaArray<Real> &bcc = pmb->pfield->bcc;
  AthenaArray<Real> &u_cr = pmb->pcr->u_cr;
  Real dt = pmb->pmy_mesh->dt;
  for (int k=pmb->ks; k<=pmb->ke; ++k) {
    for (int j=pmb->js; j<=pmb->je; ++j) {
#pragma omp simd
      for (int i=pmb->is; i<=pmb->ie; ++i) {
        Real vol = pmb->pcoord->GetCellVolume(k,j,i);
        Real face1 = pmb->pcoord->GetFace1Area(k,j,i);
        Real face2 = pmb->pcoord->GetFace2Area(k,j,i);
        Real face3 = pmb->pcoord->GetFace3Area(k,j,i);

        Real vel1_0 = cons(IM1,k,j,i) / cons(IDN,k,j,i) ;
        Real vel2_0 = cons(IM2,k,j,i) / cons(IDN,k,j,i) ;
        Real vel3_0 = cons(IM3,k,j,i) / cons(IDN,k,j,i) ;

        Real vel1_m1 = 0.5*(cons(IM1,k,j,i-1) / cons(IDN,k,j,i-1) + vel1_0);
        Real vel2_m1 = 0.5*(cons(IM2,k,j-1,i) / cons(IDN,k,j-1,i) + vel2_0) ;
        Real vel3_m1 = 0.5*(cons(IM3,k-1,j,i) / cons(IDN,k-1,j,i) + vel3_0) ;

        Real vel1_p1 = 0.5*(cons(IM1,k,j,i+1) / cons(IDN,k,j,i+1) + vel1_0) ;
        Real vel2_p1 = 0.5*(cons(IM2,k,j+1,i) / cons(IDN,k,j+1,i) + vel2_0) ;
        Real vel3_p1 = 0.5*(cons(IM3,k+1,j,i) / cons(IDN,k+1,j,i) + vel3_0) ;

    
        Real cr_p = u_cr(CRE,k,j,i)*(1.0/3.0 );
        Real myDiv = ((vel1_p1 - vel1_m1)*face1 + (vel2_p1 - vel2_m1)*face2 + (vel3_p1 - vel3_m1)*face3)/vol;
        corr += cr_p*myDiv;
        vol_tot += vol;
      }
    }
  }
  Real out = corr / vol_tot;
  return out;
}

void MeshBlock::InitUserMeshBlockData(ParameterInput *pin) {
  if (CR_ENABLED) {
    pcr->EnrollOpacityFunction(Opacity);
    pcr->EnrollStreamingFunction(Streaming);
    bool lossFlag = (pin->GetOrAddReal("problem","crLoss",0.0) > 0.0);
    if (lossFlag) {
        pcr->EnrollUserCRSource(CRSource);
    }
  }
}

void Mesh::InitUserMeshData(ParameterInput *pin) {
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank); //Just for print statements

  // Real dx =  (pin->GetReal("mesh","x1max") -  pin->GetReal("mesh","x1min") );
  // Real dy =  (pin->GetReal("mesh","x2max") -  pin->GetReal("mesh","x2min") );
  // Real dz =  (pin->GetReal("mesh","x3max") -  pin->GetReal("mesh","x3min") );
  // totalVolume = dx*dy*dz;

  turb_dedt = pin->GetOrAddReal("turbulence","dedt",0.0);
  
  if(CR_ENABLED){
    //Load CR Variables
    Real vmax = pin->GetReal("cr","vmax") ;
    Real kappaPerp = pin->GetOrAddReal("cr","kappaPerp",3e28)/(v_scale*l_scale) ;
    Real kappaParl = pin->GetOrAddReal("cr","kappaParl",3e28)/(v_scale*l_scale) ;
    sigmaPerp = vmax/(3*kappaPerp);
    sigmaParl = vmax/(3*kappaParl);
    f_i = pin->GetOrAddReal("cr","f_i",1);
    T_f_i = pin->GetOrAddReal("cr","T_f_i",10000)/T_scale;
    dT_f_i = pin->GetOrAddReal("cr","dT_f_i",1000)/T_scale;
    decouple = pin->GetOrAddReal("cr","A_decouple",1);
    crLoss = pin->GetOrAddReal("problem","crLoss",0.0);

    if (rank == 0){
      std::cout << "Vmax = " << vmax / (c / (v_scale)) << " c" << std::endl;
      std::cout << "sigmaParl = " << sigmaParl << std::endl;
      std::cout << "sigmaPerp = " << sigmaPerp << std::endl;
    }
  }
  cooling_flag = pin->GetInteger("problem","cooling");
  heating_flag = pin->GetOrAddInteger("problem","heating",1);
  if (cooling_flag != 0) {
    // EnrollUserTimeStepFunction(CoolingTimeStep);
    EnrollUserExplicitSourceFunction(mySource);
  }
  // turb_flag is initialzed in the Mesh constructor to 0 by default;
  // turb_flag = 1 for decaying turbulence
  // turb_flag = 2 for driven turbulence
  turb_flag = pin->GetInteger("problem","turb_flag");
  if (turb_flag != 0) {
#ifndef FFT
    std::stringstream msg;
    msg << "### FATAL ERROR in TurbulenceDriver::TurbulenceDriver" << std::endl
        << "non zero Turbulence flag is set without FFT!" << std::endl;
    throw std::runtime_error(msg.str().c_str());
    return;
#endif
  }

  AllocateUserHistoryOutput(7);
  EnrollUserHistoryOutput(0, TotalHeating, "totdE_heat");
  EnrollUserHistoryOutput(1, TotalHeating, "totV_heat");
  EnrollUserHistoryOutput(2, Ec_source, "totdE_cr_u");
  EnrollUserHistoryOutput(3, Ec_source, "totdE_cr_vs");
  EnrollUserHistoryOutput(4, correlation, "corr_rho_ec");
  EnrollUserHistoryOutput(5, correlation, "Lgamma_Lsun");
  EnrollUserHistoryOutput(6, correlation, "CR_Loss_Rate");
  // EnrollUserHistoryOutput(7, div_correlation, "corr_pc_div");
  return;
}

void mySource(MeshBlock *pmb, const Real time, const Real dt,
               const AthenaArray<Real> &prim, const AthenaArray<Real> &prim_scalar,
               const AthenaArray<Real> &bcc, AthenaArray<Real> &cons,
               AthenaArray<Real> &cons_scalar){
    

  Real pfloor = pmb->peos->GetPressureFloor();
  Real dfloor = pmb->peos->GetDensityFloor();
  Real Tfloor = Tlows[0]/T_scale;
  double gm1 = pmb->peos->GetGamma()-1.0;

  double totdE = 0.0;
  double totV = 0.0;
  
  for (int k=pmb->ks; k<=pmb->ke; ++k) {
    for (int j=pmb->js; j<=pmb->je; ++j) {
#pragma omp simd
      for (int i=pmb->is; i<=pmb->ie; ++i) {
        double d = cons(IDN,k,j,i);
        double p = gm1*(cons(IEN,k,j,i) - 0.5*(SQR(cons(IM1,k,j,i))+SQR(cons(IM2,k,j,i))+SQR(cons(IM3,k,j,i)))/d - 0.5*(SQR(bcc(IB1,k,j,i))+SQR(bcc(IB2,k,j,i))+SQR(bcc(IB3,k,j,i))));

        if ((d> dfloor) && (p> pfloor) ) {
          double T = p/d;
          if ((T > Tfloor)){
            double time0 = t_scale*gm1*d*n_scale*LN/(k_B*Tmax);
            double newT = invTEF(TEF(T) + dt*time0);
            double dE = d*(newT-T)/gm1;
            totdE += -1*dE * pmb->pcoord->GetCellVolume(k,j,i);
            totV += pmb->pcoord->GetCellVolume(k,j,i);
            cons(IEN,k,j,i) += dE;
          }
        }
      }
    }
  }

  if (heating_flag==1) {
    double global_totdE;
    double global_totV;
    MPI_Allreduce(&totdE, &global_totdE, 1, MPI_DOUBLE, MPI_SUM,
              MPI_COMM_WORLD);
    MPI_Allreduce(&totV, &global_totV, 1, MPI_DOUBLE, MPI_SUM,
              MPI_COMM_WORLD);


    Real turbdE =0.0;
    if (pmb->pmy_mesh->turb_flag== 3) {
      turbdE =  turb_dedt * dt ;
    } else if (pmb->pmy_mesh->turb_flag== 2) {
      turbdE = 0.0;
      Real trbTime = pmb->pmy_mesh->ptrbd->tdrive;
      Real trbDt = pmb->pmy_mesh->ptrbd->dtdrive;
      turbdE = turb_dedt *dt;
      // if ((time+dt) >= (trbTime)){
      //   turbdE =  turb_dedt * trbDt ;
      // }
    }
    // if (global_totdE <= turbdE) {
    //   std::cout << "Turbulence stronger than Heating!" << std::endl;
    // } else {
    //   global_totdE -= turbdE;
    // }


    // global_totdE -= turbdE;

    // Real heater = global_totdE / ;
    for (int k=pmb->ks; k<=pmb->ke; ++k) {
      for (int j=pmb->js; j<=pmb->je; ++j) {
  #pragma omp simd
        for (int i=pmb->is; i<=pmb->ie; ++i) {
          double d = cons(IDN,k,j,i);
          double p = gm1*(cons(IEN,k,j,i) - 0.5*(SQR(cons(IM1,k,j,i))+SQR(cons(IM2,k,j,i))+SQR(cons(IM3,k,j,i)))/d - 0.5*(SQR(bcc(IB1,k,j,i))+SQR(bcc(IB2,k,j,i))+SQR(bcc(IB3,k,j,i))));

          if ((d> dfloor) && (p> pfloor) ) {
            double T = p/d;
            if ((T > Tfloor)){
              cons(IEN,k,j,i) += (global_totdE - turbdE) / global_totV;
            }
          }
        }
      }
    }
  }
  

  return;
}

void CRSource(MeshBlock *pmb, const Real time, const Real dt,
                const AthenaArray<Real> &prim, FaceField &b, 
                AthenaArray<Real> &u_cr){ 
  for (int k=pmb->ks; k<=pmb->ke; ++k) {
    for (int j=pmb->js; j<=pmb->je; ++j) {
  #pragma omp simd
      for (int i=pmb->is; i<=pmb->ie; ++i) {
        //CRLoss Term Note that crLoss is zeta_cr total loss rate cm^3/s in computational units
        u_cr(CRE,k,j,i) -= crLoss*dt*u_cr(CRE,k,j,i)*prim(IDN,k,j,i);
      }
    }
  }
  return;
}

void MeshBlock::ProblemGenerator(ParameterInput *pin) {
  // read in the mean velocity, diffusion coefficient
  const Real n0 = pin->GetReal("problem", "n0")/n_scale; //density
  const Real T0 = pin->GetReal("problem", "T0")/T_scale;
  const Real pres = n0*T0*(n_scale*k_B*T_scale/e_scale);
  const Real gm1  = peos->GetGamma() - 1.0;
  
  const Real invbeta = pin->GetOrAddReal("problem","invbeta",0.0);
  const Real dBrat = pin->GetOrAddReal("problem","delta_B_over_B",0.0);
  const Real bx_0 = sqrt(2*invbeta*pres/(1+SQR(dBrat))); //mean field strength
  const Real b_amp = dBrat*bx_0;
  const Real invbetaCR = pin->GetOrAddReal("problem","invbetaCR",0.0);
  const Real crp = pres*invbetaCR;

  Real gamma = peos->GetGamma();
    
  // Initialize hydro variable
  for(int k=ks; k<=ke; ++k) {
    for (int j=js; j<=je; ++j) {
      for (int i=is; i<=ie; ++i) {
        Real x1 = pcoord->x1v(i);
        Real x2 = pcoord->x2v(j);
        Real x3 = pcoord->x3v(k);

        phydro->u(IDN, k, j, i) = n0;
        phydro->u(IM1, k, j, i) = 0.0;
        phydro->u(IM2, k, j, i) = 0.0;
        phydro->u(IM3, k, j, i) = 0.0;
        //energy
        if (NON_BAROTROPIC_EOS) {
            phydro->u(IEN, k, j, i) = pres/gm1;
        }

        if (CR_ENABLED) {
            pcr->u_cr(CRE,k,j,i) = 3*crp;
            pcr->u_cr(CRF1,k,j,i) = 0.0;
            pcr->u_cr(CRF2,k,j,i) = 0.0;
            pcr->u_cr(CRF3,k,j,i) = 0.0;
        }
      }
    }
  }
  //Need to set opactiy sigma in the ghost zones
  if (CR_ENABLED) {
  // Default values are 1/3
    int nz1 = block_size.nx1 + 2*(NGHOST);
    int nz2 = block_size.nx2;
    if (nz2 > 1) nz2 += 2*(NGHOST);
    int nz3 = block_size.nx3;
    if (nz3 > 1) nz3 += 2*(NGHOST);
    for(int k=0; k<nz3; ++k) {
      for(int j=0; j<nz2; ++j) {
        for(int i=0; i<nz1; ++i) {
          pcr->sigma_diff(0,k,j,i) = sigmaParl;
          pcr->sigma_diff(1,k,j,i) = sigmaPerp;
          pcr->sigma_diff(2,k,j,i) = sigmaPerp;
        }
      }
    }
  }

  
  if (MAGNETIC_FIELDS_ENABLED) {

    for (int k=ks; k<=ke; ++k) {
      for (int j=js; j<=je; ++j) {
        for (int i=is; i<=ie+1; ++i) {
          pfield->b.x1f(k,j,i) = bx0;
        }
      }
    }
    if (block_size.nx2 > 1) {
      for (int k=ks; k<=ke; ++k) {
        for (int j=js; j<=je+1; ++j) {
          for (int i=is; i<=ie; ++i) {
            pfield->b.x2f(k,j,i) =0.0 ;
          }
        }
      }
    }
    if (block_size.nx3 > 1) {
      for (int k=ks; k<=ke+1; ++k) {
        for (int j=js; j<=je; ++j) {
          for (int i=is; i<=ie; ++i) {
            pfield->b.x3f(k,j,i) = 0.0;
          }
        }
      }
    }

    // set cell centerd magnetic field
    // Add magnetic energy density to the total energy
    pfield->CalculateCellCenteredField(pfield->b,pfield->bcc,pcoord,is,ie,js,je,ks,ke);

    for(int k=ks; k<=ke; ++k) {
      for(int j=js; j<=je; ++j) {
        for(int i=is; i<=ie; ++i) {
          phydro->u(IEN,k,j,i) +=
            0.5*(SQR((pfield->bcc(IB1,k,j,i)))
               + SQR((pfield->bcc(IB2,k,j,i)))
               + SQR((pfield->bcc(IB3,k,j,i))));
        }
      }
    }
  }
  return;
}


void Opacity(MeshBlock *pmb, AthenaArray<Real> &u_cr,
               AthenaArray<Real> &prim, AthenaArray<Real> &bcc) {
  // set the default opacity to be a large value in the default hydro case
  CosmicRay *pcr=pmb->pcr;
  int kl=pmb->ks, ku=pmb->ke;
  int jl=pmb->js, ju=pmb->je;
  int il=pmb->is-1, iu=pmb->ie+1;
  if (pmb->block_size.nx2 > 1) {
    jl -= 1;
    ju += 1;
  }
  if (pmb->block_size.nx3 > 1) {
    kl -= 1;
    ku += 1;
  }

  for(int k=kl; k<=ku; ++k) {
    for(int j=jl; j<=ju; ++j) {
#pragma omp simd
      for(int i=il; i<=iu; ++i) {
        pcr->sigma_diff(0,k,j,i) = sigmaParl;
        pcr->sigma_diff(1,k,j,i) = sigmaPerp;
        pcr->sigma_diff(2,k,j,i) = sigmaPerp;
      }
    }
  }

  Real invlim=1.0/pcr->vmax;

  // The information stored in the array
  // b_angle is
  // b_angle[0]=sin_theta_b
  // b_angle[1]=cos_theta_b
  // b_angle[2]=sin_phi_b
  // b_angle[3]=cos_phi_b




  if (MAGNETIC_FIELDS_ENABLED) {
    //First, calculate B_dot_grad_Pc
    for(int k=kl; k<=ku; ++k) {
      for(int j=jl; j<=ju; ++j) {
        // x component
        pmb->pcoord->CenterWidth1(k,j,il-1,iu+1,pcr->cwidth);
        for(int i=il; i<=iu; ++i) {
          Real distance = 0.5*(pcr->cwidth(i-1) + pcr->cwidth(i+1))
                          + pcr->cwidth(i);
          Real dprdx=(u_cr(CRE,k,j,i+1) - u_cr(CRE,k,j,i-1))/3.0;
          dprdx /= distance;
          pcr->b_grad_pc(k,j,i) = bcc(IB1,k,j,i) * dprdx;
        }
        // y component
        pmb->pcoord->CenterWidth2(k,j-1,il,iu,pcr->cwidth1);
        pmb->pcoord->CenterWidth2(k,j,il,iu,pcr->cwidth);
        pmb->pcoord->CenterWidth2(k,j+1,il,iu,pcr->cwidth2);
        for(int i=il; i<=iu; ++i) {
          Real distance = 0.5*(pcr->cwidth1(i) + pcr->cwidth2(i))
                         + pcr->cwidth(i);
          Real dprdy=(u_cr(CRE,k,j+1,i) - u_cr(CRE,k,j-1,i))/3.0;
          dprdy /= distance;
          pcr->b_grad_pc(k,j,i) += bcc(IB2,k,j,i) * dprdy;
        }
        // z component
        pmb->pcoord->CenterWidth3(k-1,j,il,iu,pcr->cwidth1);
        pmb->pcoord->CenterWidth3(k,j,il,iu,pcr->cwidth);
        pmb->pcoord->CenterWidth3(k+1,j,il,iu,pcr->cwidth2);

        for(int i=il; i<=iu; ++i) {
          Real distance = 0.5*(pcr->cwidth1(i) + pcr->cwidth2(i))
                          + pcr->cwidth(i);
          Real dprdz=(u_cr(CRE,k+1,j,i) - u_cr(CRE,k-1,j,i))/3.0;
          dprdz /= distance;
          pcr->b_grad_pc(k,j,i) += bcc(IB3,k,j,i) * dprdz;

          // now only get the sign
          //  if (pcr->b_grad_pc(k,j,i) > TINY_NUMBER) pcr->b_grad_pc(k,j,i) = 1.0;
          //  else if (-pcr->b_grad_pc(k,j,i) > TINY_NUMBER) pcr->b_grad_pc(k,j,i)
          //    = -1.0;
          //  else pcr->b_grad_pc(k,j,i) = 0.0;
        }

      // now calculate the streaming velocity
      // streaming velocity is calculated with respect to the current coordinate
      //  system
      // diffusion coefficient is calculated with respect to B direction
        for(int i=il; i<=iu; ++i) {
          Real pb= bcc(IB1,k,j,i)*bcc(IB1,k,j,i)
                  +bcc(IB2,k,j,i)*bcc(IB2,k,j,i)
                  +bcc(IB3,k,j,i)*bcc(IB3,k,j,i);
          Real Temp = prim(IPR,k,j,i)/prim(IDN,k,j,i);
          Real switch_func = 0.5*(1+std::tanh( (Temp - T_f_i)/dT_f_i));
          Real my_fi = (1-f_i)*switch_func + f_i;
          // Real my_fi = std::pow(10,(1-std::log10(f_i))*switch_func + std::log10(f_i));
          Real inv_sqrt_rho = 1.0/std::sqrt(prim(IDN,k,j,i) * my_fi);
          Real va1 = bcc(IB1,k,j,i)*inv_sqrt_rho;
          Real va2 = bcc(IB2,k,j,i)*inv_sqrt_rho;
          Real va3 = bcc(IB3,k,j,i)*inv_sqrt_rho;


          Real va = std::sqrt(SQR(va1) + SQR(va2) + SQR(va3));

          Real dpc_sign = 0.0;
          if (pcr->b_grad_pc(k,j,i) > TINY_NUMBER) dpc_sign = 1.0;
          else if (-pcr->b_grad_pc(k,j,i) > TINY_NUMBER) dpc_sign = -1.0;
          if (pcr->stream_flag > 0) {
            pcr->v_adv(0,k,j,i) = -va1 * dpc_sign;
            pcr->v_adv(1,k,j,i) = -va2 * dpc_sign;
            pcr->v_adv(2,k,j,i) = -va3 * dpc_sign;
          } else {
            pcr->v_adv(0,k,j,i) = 0.0;
            pcr->v_adv(1,k,j,i) = 0.0;
            pcr->v_adv(2,k,j,i) = 0.0;
          }

          // now the diffusion coefficient
          if (pcr->stream_flag > 0) {
            if (va < TINY_NUMBER) {
              pcr->sigma_adv(0,k,j,i) = pcr->max_opacity;
            } else {
              pcr->sigma_adv(0,k,j,i) = std::abs(pcr->b_grad_pc(k,j,i))
                            /(std::sqrt(pb)* va * decouple * (1.0 + 1.0/3.0)
                                      * invlim * u_cr(CRE,k,j,i));
            }

            pcr->sigma_adv(1,k,j,i) = pcr->max_opacity;
            pcr->sigma_adv(2,k,j,i) = pcr->max_opacity;
          } else {
            pcr->sigma_adv(0,k,j,i)  = pcr->max_opacity;
            pcr->sigma_adv(1,k,j,i)  = pcr->max_opacity;
            pcr->sigma_adv(2,k,j,i)  = pcr->max_opacity;
          }
          // Now calculate the angles of B
          Real bxby = std::sqrt(bcc(IB1,k,j,i)*bcc(IB1,k,j,i) +
                           bcc(IB2,k,j,i)*bcc(IB2,k,j,i));
          Real btot = std::sqrt(pb);
          if (btot > TINY_NUMBER) {
            pcr->b_angle(0,k,j,i) = bxby/btot;
            pcr->b_angle(1,k,j,i) = bcc(IB3,k,j,i)/btot;
          } else {
            pcr->b_angle(0,k,j,i) = 1.0;
            pcr->b_angle(1,k,j,i) = 0.0;
          }
          if (bxby > TINY_NUMBER) {
            pcr->b_angle(2,k,j,i) = bcc(IB2,k,j,i)/bxby;
            pcr->b_angle(3,k,j,i) = bcc(IB1,k,j,i)/bxby;
          } else {
            pcr->b_angle(2,k,j,i) = 0.0;
            pcr->b_angle(3,k,j,i) = 1.0;
          }
        }
      }
    }
  }
}

void Streaming(MeshBlock *pmb, AthenaArray<Real> &u_cr,
             AthenaArray<Real> &prim, AthenaArray<Real> &bcc,
             AthenaArray<Real> &grad_pc, int k, int j, int is, int ie) {
  CosmicRay *pcr=pmb->pcr;
  Real invlim = 1.0/pcr->vmax;

  for(int i=is; i<=ie; ++i) {
    Real Temp = prim(IPR,k,j,i)/prim(IDN,k,j,i);
    Real switch_func = 0.5*(1+std::tanh( (Temp - T_f_i)/dT_f_i));
    Real my_fi = (1-f_i)*switch_func + f_i;
    // Real my_fi = std::pow(10,(1-std::log10(f_i))*switch_func + std::log10(f_i));
    Real inv_sqrt_rho = 1.0/std::sqrt(prim(IDN,k,j,i) * my_fi);
    Real bsq = bcc(IB1,k,j,i)*bcc(IB1,k,j,i)
              +bcc(IB2,k,j,i)*bcc(IB2,k,j,i)
              +bcc(IB3,k,j,i)*bcc(IB3,k,j,i);

    Real b_grad_pc = bcc(IB1,k,j,i) * grad_pc(0,k,j,i)
                   + bcc(IB2,k,j,i) * grad_pc(1,k,j,i)
                   + bcc(IB3,k,j,i) * grad_pc(2,k,j,i);

    Real va1 = bcc(IB1,k,j,i) * inv_sqrt_rho;
    Real va2 = bcc(IB2,k,j,i) * inv_sqrt_rho;
    Real va3 = bcc(IB3,k,j,i) * inv_sqrt_rho;

    Real va = std::sqrt(bsq) * inv_sqrt_rho;
    Real dpc_sign = 0.0;

    if (b_grad_pc > TINY_NUMBER) dpc_sign = 1.0;
    else if (-b_grad_pc > TINY_NUMBER) dpc_sign = -1.0;

    if (pcr->stream_flag > 0) {
      pcr->v_adv(0,k,j,i) = -va1 * dpc_sign;
      pcr->v_adv(1,k,j,i) = -va2 * dpc_sign;
      pcr->v_adv(2,k,j,i) = -va3 * dpc_sign;
      if (va > TINY_NUMBER) {
        pcr->sigma_adv(0,k,j,i) = std::abs(b_grad_pc)/(std::sqrt(bsq) * va * decouple *
                               (4.0/3.0) * invlim * u_cr(CRE,k,j,i));
        pcr->sigma_adv(1,k,j,i) = pcr->max_opacity;
        pcr->sigma_adv(2,k,j,i) = pcr->max_opacity;
      }
    } else {
      pcr->v_adv(0,k,j,i) = 0.0;
      pcr->v_adv(1,k,j,i) = 0.0;
      pcr->v_adv(2,k,j,i) = 0.0;
      pcr->sigma_adv(0,k,j,i)  = pcr->max_opacity;
      pcr->sigma_adv(1,k,j,i)  = pcr->max_opacity;
      pcr->sigma_adv(2,k,j,i)  = pcr->max_opacity;
    }
  }
}
