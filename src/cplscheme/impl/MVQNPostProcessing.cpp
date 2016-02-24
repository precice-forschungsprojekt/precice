/*
 * MVQNPostProcessing.cpp
 *
 *  Created on: Dez 5, 2015
 *      Author: Klaudius Scheufele
 */


#ifndef PRECICE_NO_MPI

// Copyright (C) 2015 Universität Stuttgart
// This file is part of the preCICE project. For conditions of distribution and
// use, please see the license notice at http://www5.in.tum.de/wiki/index.php/PreCICE_License
#include "MVQNPostProcessing.hpp"
#include "cplscheme/CouplingData.hpp"
#include "utils/Globals.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/Vertex.hpp"
#include "utils/Dimensions.hpp"
#include "utils/MasterSlave.hpp"
#include "utils/EventTimings.hpp"
#include "utils/EigenHelperFunctions.hpp"
#include "com/MPIPortsCommunication.hpp"
#include "com/Communication.hpp"
#include "Eigen/Dense"

#include <time.h>
#include <sstream>
#include <fstream>
#include <cstring>

#include <thread>
#include <chrono>
//#include "utils/NumericalCompare.hpp"

using precice::utils::Event;


namespace precice {
namespace cplscheme {
namespace impl {

 //tarch::logging::Log MVQNPostProcessing::_log("precice::cplscheme::impl::MVQNPostProcessing");


// ==================================================================================
MVQNPostProcessing:: MVQNPostProcessing
(
  double initialRelaxation,
  bool forceInitialRelaxation,
  int    maxIterationsUsed,
  int    timestepsReused,
  int 	 filter,
  double singularityLimit,
  std::vector<int> dataIDs,
  PtrPreconditioner preconditioner,
  bool   alwaysBuildJacobian,
  int    imvjRestartType,
  int    chunkSize,
  int    RSLSreusedTimesteps,
  double RSSVDtruncationEps)
:
  BaseQNPostProcessing(initialRelaxation, forceInitialRelaxation, maxIterationsUsed, timestepsReused,
		       filter, singularityLimit, dataIDs, preconditioner),
//  _secondaryOldXTildes(),
  _invJacobian(),
  _oldInvJacobian(),
  _Wtil(),
  _WtilChunk(),
  _pseudoInverseChunk(),
  _matrixV_RSLS(),
  _matrixW_RSLS(),
  _matrixCols_RSLS(),
  _cyclicCommLeft(nullptr),
  _cyclicCommRight(nullptr),
  _parMatrixOps(nullptr),
  _svdJ(RSSVDtruncationEps, preconditioner),
  _alwaysBuildJacobian(alwaysBuildJacobian),
  _imvjRestartType(imvjRestartType),
  _imvjRestart(false),
  _chunkSize(chunkSize),
  _RSLSreusedTimesteps(RSLSreusedTimesteps),
  _usedColumnsPerTstep(5),
  _nbRestarts(0),
  //_info2(),
  _avgRank(0)
{}

// ==================================================================================
MVQNPostProcessing::~MVQNPostProcessing()
{
	//if(utils::MasterSlave::_masterMode ||utils::MasterSlave::_slaveMode){ // not possible because of tests, MasterSlave is deactivated when PP is killed

	// close and shut down cyclic communication connections
	if(_cyclicCommRight != nullptr || _cyclicCommLeft != nullptr){
		if((utils::MasterSlave::_rank % 2) == 0)
		{
		  _cyclicCommLeft->closeConnection();
		  _cyclicCommRight->closeConnection();
		}else{
		  _cyclicCommRight->closeConnection();
		  _cyclicCommLeft->closeConnection();
		}
		_cyclicCommRight = nullptr;
		_cyclicCommLeft = nullptr;
	}
}

// ==================================================================================
void MVQNPostProcessing:: initialize
(
  DataMap& cplData )
{
  preciceTrace(__func__);
  Event e("MVQNPostProcessing::initialize()", true, true); // time measurement, barrier
  // do common QN post processing initialization
  BaseQNPostProcessing::initialize(cplData);
  
  //std::stringstream sss;
  //sss<<"residualWeights_rank--"<<utils::MasterSlave::_rank;
  //_info2.open(sss.str(), std::ios_base::out);
  //_info2 << std::setprecision(16);

  if (_imvjRestartType > 0)
    _imvjRestart = true;


  if(utils::MasterSlave::_masterMode ||utils::MasterSlave::_slaveMode){
		/*
		 * TODO: FIXME: This is a temporary and hacky realization of the cyclic commmunication between slaves
		 * 				Therefore the requesterName and accessorName are not given (cf solverInterfaceImpl).
		 * 				The master-slave communication should be modified such that direct communication between
		 * 				slaves is possible (via MPIDirect)
		 */


		 _cyclicCommLeft = com::Communication::SharedPointer(new com::MPIPortsCommunication("."));
		 _cyclicCommRight = com::Communication::SharedPointer(new com::MPIPortsCommunication("."));

		 // initialize cyclic communication between successive slaves
		int prevProc = (utils::MasterSlave::_rank-1 < 0) ? utils::MasterSlave::_size-1 : utils::MasterSlave::_rank-1;
		if((utils::MasterSlave::_rank % 2) == 0)
		{
		  _cyclicCommLeft->acceptConnection("cyclicComm-" + std::to_string(prevProc), "", 0, 1 );
		  _cyclicCommRight->requestConnection("cyclicComm-" +  std::to_string(utils::MasterSlave::_rank), "", 0, 1 );
		}else{
		  _cyclicCommRight->requestConnection("cyclicComm-" +  std::to_string(utils::MasterSlave::_rank), "", 0, 1 );
		  _cyclicCommLeft->acceptConnection("cyclicComm-" + std::to_string(prevProc), "", 0, 1 );
		}
  }

  // initialize parallel matrix-matrix operation module
  _parMatrixOps = impl::PtrParMatrixOps(new impl::ParallelMatrixOperations());
  _parMatrixOps->initialize(_cyclicCommLeft, _cyclicCommRight);
  _svdJ.initialize(_parMatrixOps, getLSSystemRows());

  int entries = _residuals.size();
  int global_n = 0;

	if (not utils::MasterSlave::_masterMode && not utils::MasterSlave::_slaveMode) {
		global_n = entries;
	}else{
		global_n = _dimOffsets.back();
	}
  
	// only need memory for Jacobain of not in restart mode
	if(not _imvjRestart){
	  _invJacobian = Eigen::MatrixXd::Zero(global_n, entries);
	  _oldInvJacobian = Eigen::MatrixXd::Zero(global_n, entries);
	}
	// initialize V, W matrices for the LS restart
	if(_imvjRestartType == RS_LS){
	  _matrixCols_RSLS.push_front(0);
	  _matrixV_RSLS = Eigen::MatrixXd::Zero(entries, 0);
	  _matrixW_RSLS = Eigen::MatrixXd::Zero(entries, 0);
	}
  _Wtil = Eigen::MatrixXd::Zero(entries, 0);
  _preconditioner->triggerGlobalWeights(global_n);

  if (utils::MasterSlave::_masterMode || (not utils::MasterSlave::_masterMode && not utils::MasterSlave::_slaveMode))
    _infostream<<" IMVJ restart mode: "<<_imvjRestart<<"\n chunk size: "<<_chunkSize<<"\n trunc eps: "<<_svdJ.getThreshold()<<"\n R_RS: "<<_RSLSreusedTimesteps<<"\n--------\n"<<std::endl;
}

// ==================================================================================
void MVQNPostProcessing::computeUnderrelaxationSecondaryData
(
  DataMap& cplData)
{
    //Store x_tildes for secondary data
  //  foreach (int id, _secondaryDataIDs){
  //    assertion2(_secondaryOldXTildes[id].size() == cplData[id]->values->size(),
  //               _secondaryOldXTildes[id].size(), cplData[id]->values->size());
  //    _secondaryOldXTildes[id] = *(cplData[id]->values);
  //  }

    // Perform underrelaxation with initial relaxation factor for secondary data
    for (int id: _secondaryDataIDs){
      PtrCouplingData data = cplData[id];
      Eigen::VectorXd& values = *(data->values);
      values *= _initialRelaxation;                     // new * omg
      Eigen::VectorXd& secResiduals = _secondaryResiduals[id];
      secResiduals = data->oldValues.col(0);            // old
      secResiduals *= 1.0 - _initialRelaxation;         // (1-omg) * old
      values += secResiduals;                           // (1-omg) * old + new * omg
    }
}

// ==================================================================================
void MVQNPostProcessing::updateDifferenceMatrices
(
    DataMap& cplData)
{
  /**
   *  Matrices and vectors used in this method as well as the result Wtil are
   *  NOT SCALED by the preconditioner.
   */

  preciceTrace(__func__);
  Event e(__func__, true, true); // time measurement, barrier

  // call the base method for common update of V, W matrices
  // important that base method is called before updating _Wtil
  BaseQNPostProcessing::updateDifferenceMatrices(cplData);

  // update _Wtil if the efficient computation of the quasi-Newton update is used
  // or update current Wtil if the restart mode of imvj is used
  if(not _alwaysBuildJacobian || _imvjRestart)
  {
    if (_firstIteration && (_firstTimeStep || _forceInitialRelaxation)) {
      // do nothing: constant relaxation
    } else {
      if (not _firstIteration) {
        // Update matrix _Wtil = (W - J_prev*V) with newest information

        Eigen::VectorXd v = _matrixV.col(0);
        Eigen::VectorXd w = _matrixW.col(0);

        // here, we check for _Wtil.cols() as the matrices V, W need to be updated before hand
        // and thus getLSSystemCols() does not yield the correct result.
        bool columnLimitReached = _Wtil.cols() == _maxIterationsUsed;
        bool overdetermined = _Wtil.cols() <= getLSSystemRows();

        Eigen::VectorXd wtil = Eigen::VectorXd::Zero(_matrixV.rows());

        // add column: Wtil(:,0) = W(:,0) - sum_q [ Wtil^q * ( Z^q * V(:,0)) ]
        //                                         |--- J_prev ---|
        // iterate over all stored Wtil and Z matrices in current chunk
        if(_imvjRestart){
          for(int i = 0; i < (int)_WtilChunk.size(); i++){
            int colsLSSystemBackThen = _pseudoInverseChunk[i].rows();
            assertion2(colsLSSystemBackThen == _WtilChunk[i].cols(), colsLSSystemBackThen, _WtilChunk[i].cols());
            Eigen::VectorXd Zv = Eigen::VectorXd::Zero(colsLSSystemBackThen);
            // multiply: Zv := Z^q * V(:,0) of size (m x 1)
            _parMatrixOps->multiply(_pseudoInverseChunk[i], v, Zv, colsLSSystemBackThen, getLSSystemRows(), 1);
            // multiply: Wtil^q * Zv  dimensions: (n x m) * (m x 1), fully local
            wtil += _WtilChunk[i] * Zv;
          }

          // store columns if restart mode = RS-LS
          if(_imvjRestartType == RS_LS){
            if(_matrixCols_RSLS.front() < _usedColumnsPerTstep){
              utils::appendFront(_matrixV_RSLS, v);
              utils::appendFront(_matrixW_RSLS, w);
              _matrixCols_RSLS.front()++;
            }
          }

        // imvj without restart is used, but efficient update, i.e. no Jacobian assembly in each iteration
        // add column: Wtil(:,0) = W(:,0) - J_prev * V(:,0)
        }else{
          // compute J_prev * V(0) := wtil the new column in _Wtil of dimension: (n x n) * (n x 1) = (n x 1),
          //                                        parallel: (n_global x n_local) * (n_local x 1) = (n_local x 1)
          _parMatrixOps->multiply(_oldInvJacobian, v, wtil, _dimOffsets, getLSSystemRows(), getLSSystemRows(), 1, false);
        }
        wtil *= -1;
        wtil += w;

        if (not columnLimitReached && overdetermined) {
          utils::appendFront(_Wtil, wtil);
        }else {
          utils::shiftSetFirst(_Wtil, wtil);
        }
      }
    }
  }
}


// ==================================================================================
void MVQNPostProcessing::computeQNUpdate(
     PostProcessing::DataMap& cplData,
     Eigen::VectorXd& xUpdate)
{
  /**
   * The inverse Jacobian
   *
   *        J_inv = J_inv_n + (W - J_inv_n*V)*(V^T*V)^-1*V^T
   *
   * is computed and the resulting quasi-Newton update is returned.
   * Used matrices (V, W, Wtil, invJacobian, oldINvJacobian) are
   * scaled with the used preconditioner.
   */

  preciceTrace(__func__);
  Event e(__func__, true, true); // time measurement, barrier
  preciceDebug("compute IMVJ quasi-Newton update");


  //_info2<<_preconditioner->getWeights().front()<<", "<<_preconditioner->getWeights().back()<<", "<<_preconditioner->getWeights().front()/_preconditioner->getWeights().back()<<";"<<std::endl;

  Event ePrecond_1("preconditioning of J", true, true); // ------ time measurement, barrier

  // Wtil needs to be preconditioned if it is not re-built from scratch, i.e., if either
  // the efficient IMVJ update or the IMVJ restart mode is used
  if((not _alwaysBuildJacobian || _imvjRestart) && (_Wtil.size() > 0)){
    _preconditioner->apply(_Wtil);
  }

  // if imvj is used in restart mode, all stored matrices Wtil^q and Z^q within the
  // current chunk need to be scaled with the current preconditioner weights
  if(_imvjRestart){
    // see below how J needs to be scaled. If this is applied to the sub-blocks of the
    // Jacobian, the following preconditioning of the local matrices Wtil^q and Z^q falls out.
    // [Wtil^q]' := P * Wtil^q      and     [Z^q]' := Z^q * P^-1
    for(int i = 0; i < (int)_WtilChunk.size(); i++){
      _preconditioner->apply(_WtilChunk[i]);
      _preconditioner->revert(_pseudoInverseChunk[i], true, false);
    }

    // if imvj is used in no-restart mode, the full matrix J needs to be preconditioned
  }else{
    // J needs to be scaled as follows:       J' := P * J * P^-1
    // where P = diag(P1,P2,...) and Pi = diag(w1i, w2i, ..).
    // Thus, P^-1 needs the weights from all procs, i.e., global weights.
    _preconditioner->apply(_oldInvJacobian,false);
    _preconditioner->revert(_oldInvJacobian,true);
  }
  ePrecond_1.stop();                                    // ------

  // either compute efficient, omitting to build the Jacobian in each iteration or inefficient.
  if(_alwaysBuildJacobian){
    computeNewtonUpdate(cplData, xUpdate);
  }else{
    computeNewtonUpdateEfficient(cplData, xUpdate);
  }

  Event ePrecond_2("preconditioning of J", true, true); // ------ time measurement, barrier
  if((not _alwaysBuildJacobian || _imvjRestart) && (_Wtil.size() > 0)){
    _preconditioner->revert(_Wtil);
  }

  if(_imvjRestart){
    for(int i = 0; i < (int)_WtilChunk.size(); i++){
       _preconditioner->revert(_WtilChunk[i]);
       _preconditioner->apply(_pseudoInverseChunk[i], true, false);
     }
  }else{
    _preconditioner->revert(_oldInvJacobian,false);
    _preconditioner->apply(_oldInvJacobian,true);
  }
  ePrecond_2.stop();                                    // ------
}


// ==================================================================================
void MVQNPostProcessing::pseudoInverse(
    Eigen::MatrixXd& pseudoInverse)
{
  preciceTrace(__func__);
  /**
   *   computation of pseudo inverse matrix Z = (V^TV)^-1 * V^T as solution
   *   to the equation R*z = Q^T(i) for all columns i,  via back substitution.
   */
  auto Q = _qrV.matrixQ();
  auto R = _qrV.matrixR();

  assertion2(pseudoInverse.rows() == _qrV.cols(), pseudoInverse.rows(), _qrV.cols());
  assertion2(pseudoInverse.cols() == _qrV.rows(), pseudoInverse.cols(), _qrV.rows());

  Event e(__func__, true, true); // time measurement, barrier
  Eigen::VectorXd yVec(pseudoInverse.rows());

  // assertions for the case of processors with no vertices
  if(!_hasNodesOnInterface){
      assertion2(_qrV.cols() == getLSSystemCols(), _qrV.cols(), getLSSystemCols()); assertion1(_qrV.rows() == 0, _qrV.rows()); assertion1(Q.size() == 0, Q.size());
  }

  // backsubstitution
  Event e_qr("solve Z = (V^TV)^-1V^T via QR", true, true); // ------- time measurement, barrier
  for (int i = 0; i < Q.rows(); i++) {
    Eigen::VectorXd Qrow = Q.row(i);
    yVec = R.triangularView<Eigen::Upper>().solve<Eigen::OnTheLeft>(Qrow);
    pseudoInverse.col(i) = yVec;
  }
  e_qr.stop();                                            // ----------------
}

// ==================================================================================
void MVQNPostProcessing::buildWtil()
{
  /**
   * PRECONDITION: Assumes that V, W, J_prev are already preconditioned,
   */
  preciceTrace(__func__);
  Event e_WtilV("compute W_til = (W - J_prev*V)", true, true); // time measurement, barrier
  assertion2(_matrixV.rows() == _qrV.rows(), _matrixV.rows(), _qrV.rows());  assertion2(getLSSystemCols() == _qrV.cols(), getLSSystemCols(), _qrV.cols());

  _Wtil = Eigen::MatrixXd::Zero(_qrV.rows(), _qrV.cols());

  // imvj restart mode: re-compute Wtil: Wtil = W - sum_q [ Wtil^q * (Z^q*V) ]
  //                                                      |--- J_prev ---|
  // iterate over all stored Wtil and Z matrices in current chunk
  if(_imvjRestart){
    for(int i = 0; i < (int)_WtilChunk.size(); i++){
      int colsLSSystemBackThen = _pseudoInverseChunk[i].rows();
      assertion2(colsLSSystemBackThen == _WtilChunk[i].cols(), colsLSSystemBackThen, _WtilChunk[i].cols());
      Eigen::MatrixXd ZV = Eigen::MatrixXd::Zero(colsLSSystemBackThen, _qrV.cols());
      // multiply: ZV := Z^q * V of size (m x m) with m=#cols, stored on each proc.
      _parMatrixOps->multiply(_pseudoInverseChunk[i], _matrixV, ZV, colsLSSystemBackThen, getLSSystemRows(), _qrV.cols());
      // multiply: Wtil^q * ZV  dimensions: (n x m) * (m x m), fully local and embarrassingly parallel
      _Wtil += _WtilChunk[i] * ZV;
    }

  // imvj without restart is used, i.e., recompute Wtil: Wtil = W - J_prev * V
  }else{
    // multiply J_prev * V = W_til of dimension: (n x n) * (n x m) = (n x m),
    //                                    parallel:  (n_global x n_local) * (n_local x m) = (n_local x m)
    _parMatrixOps->multiply(_oldInvJacobian, _matrixV, _Wtil, _dimOffsets, getLSSystemRows(), getLSSystemRows(), getLSSystemCols(), false);
  }

  // W_til = (W-J_inv_n*V) = (W-V_tilde)
  _Wtil *= -1.;
  _Wtil = _Wtil + _matrixW;

  _resetLS = false;
  e_WtilV.stop();
}

// ==================================================================================
void MVQNPostProcessing::buildJacobian()
{
  preciceTrace(__func__);
  /**      --- compute inverse Jacobian ---
  *
  * J_inv = J_inv_n + (W - J_inv_n*V)*(V^T*V)^-1*V^T
  *
  * assumes that J_prev, V, W are already preconditioned
  */

  /**
   *  (1) computation of pseudo inverse Z = (V^TV)^-1 * V^T
   */
  Eigen::MatrixXd Z(_qrV.cols(), _qrV.rows());
  pseudoInverse(Z);

  /**
  *  (2) Multiply J_prev * V =: W_tilde
  */
  assertion2(_matrixV.rows() == _qrV.rows(), _matrixV.rows(), _qrV.rows());  assertion2(getLSSystemCols() == _qrV.cols(), getLSSystemCols(), _qrV.cols());
  if(_resetLS){
    buildWtil();
    preciceWarning(__func__," ATTENTION, in buildJacobian call for buildWtill() - this should not be the case except the coupling did only one iteration");
  }

  /** (3) compute invJacobian = W_til*Z
  *
  *  where Z = (V^T*V)^-1*V^T via QR-dec and back-substitution       dimension: (n x n) * (n x m) = (n x m),
  *  and W_til = (W - J_inv_n*V)                                     parallel:  (n_global x n_local) * (n_local x m) = (n_local x m)
  */
  Event e_WtilZ("compute J = W_til*Z", true, true); // -------- time measurement, barrier

  _parMatrixOps->multiply(_Wtil, Z, _invJacobian, _dimOffsets, getLSSystemRows(), getLSSystemCols(), getLSSystemRows());
  e_WtilZ.stop();                                   // --------

  // update Jacobian
  _invJacobian = _invJacobian + _oldInvJacobian;
}

// ==================================================================================
void MVQNPostProcessing::computeNewtonUpdateEfficient(
    PostProcessing::DataMap& cplData,
    Eigen::VectorXd& xUpdate)
{
  preciceTrace(__func__);

  /**      --- update inverse Jacobian efficient, ---
  *   If normal mode is used:
  *   Do not recompute W_til in every iteration and do not build
  *   the entire Jacobian matrix. This is only necessary if the coupling
  *   iteration has converged, namely in the last iteration.
  *
  *   If restart-mode is used:
  *   The Jacobian is never build. Store matrices Wtil^q and Z^q for the last M time steps.
  *   After M time steps, a restart algorithm is performed basedon the restart-mode type, either
  *   Least-Squares restart (IQN-ILS like) or maintaining of a updated truncated SVD decomposition
  *   of the SVD.
  *
  * J_inv = J_inv_n + (W - J_inv_n*V)*(V^T*V)^-1*V^T
  *
  * ASSUMPTION: All objects are scaled with the active preconditioner
  */

  /**
   *  (1) computation of pseudo inverse Z = (V^TV)^-1 * V^T
   */
  Eigen::MatrixXd Z(_qrV.cols(), _qrV.rows());
  pseudoInverse(Z);

  /**
  *  (2) Construction of _Wtil = (W - J_prev * V), should be already present due to updated computation
  */
  assertion2(_matrixV.rows() == _qrV.rows(), _matrixV.rows(), _qrV.rows());  assertion2(getLSSystemCols() == _qrV.cols(), getLSSystemCols(), _qrV.cols());

  // rebuild matrix Wtil if V changes substantially.
  if(_resetLS){
    buildWtil();
  }

  /**
   *  Avoid computation of Z*Wtil = Jtil \in (n x n). Rather do matrix-vector computations
   *  [ J_prev*(-res) ] + [Wtil * [Z * (-res)] ]
   *  '----- 1 -------'           '----- 2 ----'
   *                      '-------- 3 ---------'
   *
   *  (3) compute r_til = Z*(-residual)   where Z = (V^T*V)^-1*V^T via QR-dec and back-substitution
   *
   *  dimension: (m x n) * (n x 1) = (m x 1),
   *  parallel:  (m x n_local) * (n x 1) = (m x 1)
   */
  Event e_Zr("compute r_til = Z*(-res)", true, true); // -------- time measurement, barrier
  Eigen::VectorXd negativeResiduals = - _residuals;
  Eigen::VectorXd r_til = Eigen::VectorXd::Zero(getLSSystemCols());
  _parMatrixOps->multiply(Z, negativeResiduals, r_til, getLSSystemCols(), getLSSystemRows(), 1);
  e_Zr.stop();                                        // --------

  /**
   * (4) compute _Wtil * r_til
   *
   * dimension: (n x m) * (m x 1) = (n x 1),
   * parallel:  (n_local x m) * (m x 1) = (n_local x 1)
   *
   * Note: r_til is not distributed but locally stored on each proc (dimension m x 1)
   */
  Eigen::VectorXd xUptmp(_residuals.size());
  xUpdate = Eigen::VectorXd::Zero(_residuals.size());
  xUptmp = _Wtil * r_til;                      // local product, result is naturally distributed.

  /**
   *  (5) xUp = J_prev * (-res) + Wtil*Z*(-res)
   *
   *  restart-mode: sum_q { Wtil^q * [ Z^q * (-res) ] },
   *  where r_til = Z^q * (-res) is computed first and then xUp := Wtil^q * r_til
   */
  Event e_Jpr("compute xUp(1) = J_prev*(-res)", true, true); // -------- time measurement, barrier

  if(_imvjRestart){
    for(int i = 0; i < (int)_WtilChunk.size(); i++){
      int colsLSSystemBackThen = _pseudoInverseChunk[i].rows();
      assertion2(colsLSSystemBackThen == _WtilChunk[i].cols(), colsLSSystemBackThen, _WtilChunk[i].cols());
      r_til = Eigen::VectorXd::Zero(colsLSSystemBackThen);
      // multiply: r_til := Z^q * (-res) of size (m x 1) with m=#cols of LS at that time, result stored on each proc.
      _parMatrixOps->multiply(_pseudoInverseChunk[i], negativeResiduals, r_til, colsLSSystemBackThen, getLSSystemRows(), 1);
      // multiply: Wtil^q * r_til  dimensions: (n x m) * (m x 1), fully local and embarrassingly parallel
      xUpdate += _WtilChunk[i] * r_til;
    }

  // imvj without restart is used, i.e., compute directly J_prev * (-res)
  }else{
    _parMatrixOps->multiply(_oldInvJacobian, negativeResiduals, xUpdate, _dimOffsets, getLSSystemRows(), getLSSystemRows(), 1, false);
    preciceDebug("Mult J*V DONE");
  }
  e_Jpr.stop();                                              // --------

  xUpdate += xUptmp;

  // pending deletion: delete Wtil
  if (_firstIteration && _timestepsReused == 0 && not _forceInitialRelaxation) {
    _Wtil.conservativeResize(0,0);
    _resetLS = true;
  }
}

// ==================================================================================
void MVQNPostProcessing::computeNewtonUpdate
(PostProcessing::DataMap& cplData, Eigen::VectorXd& xUpdate)
{
	preciceTrace(__func__);

	/**      --- update inverse Jacobian ---
	*
	* J_inv = J_inv_n + (W - J_inv_n*V)*(V^T*V)^-1*V^T
	*/
  
	/**  (1) computation of pseudo inverse Z = (V^TV)^-1 * V^T
   */
  Eigen::MatrixXd Z(_qrV.cols(), _qrV.rows());
  pseudoInverse(Z);

	/**  (2) Multiply J_prev * V =: W_tilde
	*/
	buildWtil();

	/**  (3) compute invJacobian = W_til*Z
	*
	*  where Z = (V^T*V)^-1*V^T via QR-dec and back-substitution             dimension: (n x n) * (n x m) = (n x m),
	*  and W_til = (W - J_inv_n*V)                                           parallel:  (n_global x n_local) * (n_local x m) = (n_local x m)
	*/
	Event e_WtilZ("compute J = W_til*Z", true, true); // -------- time measurement, barrier

	_parMatrixOps->multiply(_Wtil, Z, _invJacobian, _dimOffsets, getLSSystemRows(), getLSSystemCols(), getLSSystemRows());
	e_WtilZ.stop();                                   // --------

	// update Jacobian
	_invJacobian = _invJacobian + _oldInvJacobian;

	/**  (4) solve delta_x = - J_inv * res
	 */
	Event e_up("compute update = J*(-res)", true, true); // -------- time measurement, barrier
	Eigen::VectorXd negativeResiduals = - _residuals;

	// multiply J_inv * (-res) = x_Update of dimension: (n x n) * (n x 1) = (n x 1),
	//                                        parallel: (n_global x n_local) * (n_local x 1) = (n_local x 1)
	_parMatrixOps->multiply(_invJacobian, negativeResiduals, xUpdate, _dimOffsets, getLSSystemRows(), getLSSystemRows(), 1, false);
  e_up.stop();                                         // --------
}

// ==================================================================================
void MVQNPostProcessing::restartIMVJ()
{
  preciceTrace(__func__);
  int used_storage = 0;
  int theoreticalJ_storage = 2*getLSSystemRows()*_residuals.size() + 3*_residuals.size()*getLSSystemCols() + _residuals.size()*_residuals.size();
  //               ------------ RESTART SVD ------------
  if(_imvjRestartType == MVQNPostProcessing::RS_SVD)
  {
    int rankBefore = _svdJ.isSVDinitialized() ? _svdJ.rank() : 0;

    // INVARIANT: all objects are unscaled and entry and unscaled at exit

    // if it is the first time step, there is no initial SVD, so take all Wtil, Z matrices
    // otherwise, the first element of each container holds the decomposition of the current
    // truncated SVD, i.e., Wtil^0 = \phi, Z^0 = S\psi^T, this should not be added to the SVD.
    int q = _svdJ.isSVDinitialized() ? 1 : 0;

    // perform M-1 rank-1 updates of the truncated SVD-dec of the Jacobian
    for(; q < (int)_WtilChunk.size(); q++){
      // update SVD, i.e., PSI * SIGMA * PHI^T <-- PSI * SIGMA * PHI^T + Wtil^q * Z^q
      _svdJ.update(_WtilChunk[q], _pseudoInverseChunk[q].transpose());
      used_storage += 2*_WtilChunk.size();
    }
    int m = _WtilChunk[q].cols(), n = _WtilChunk[q].rows();
    used_storage += 2*rankBefore*m + 4*m*n + 2*m*m + (rankBefore+m)*(rankBefore+m) + 2*n*(rankBefore+m);

    // drop all stored Wtil^q, Z^q matrices
    _WtilChunk.clear();
    _pseudoInverseChunk.clear();

    auto& psi = _svdJ.matrixPsi();
    auto& sigma = _svdJ.singularValues();
    auto& phi = _svdJ.matrixPhi();

    // multiply sigma * phi^T, phi is distributed block-row wise, phi^T is distributed block-column wise
    // sigma is stored local on each proc, thus, the multiplication is fully local, no communication.
    // Z = sigma * phi^T
    Eigen::MatrixXd Z(phi.cols(), phi.rows());
    for(int i=0; i < (int)Z.rows(); i++)
       for(int j=0; j < (int)Z.cols(); j++)
         Z(i,j) = phi(j,i) * sigma[i];

    int rankAfter = _svdJ.rank();
    int waste = _svdJ.getWaste();
    _avgRank += rankAfter;

    // store factorized truncated SVD of J
    _WtilChunk.push_back(psi);
    _pseudoInverseChunk.push_back(Z);

    preciceDebug("MVJ-RESTART, mode=SVD. Rank of truncated SVD of Jacobian "<<rankAfter<<", new modes: "<<rankAfter-rankBefore<<", truncated modes: "<<waste<<" avg rank: "<<_avgRank/tSteps);
    double percentage = 100.0*used_storage/(double)theoreticalJ_storage;
    if (utils::MasterSlave::_masterMode || (not utils::MasterSlave::_masterMode && not utils::MasterSlave::_slaveMode))
      _infostream<<" - MVJ-RESTART " <<_nbRestarts<<", mode= SVD -\n  new modes: "<<rankAfter-rankBefore<<"\n  rank svd: "<<rankAfter<<"\n  avg rank: "<<_avgRank/tSteps<<"\n  truncated modes: "<<waste<<"\n  used storage: "<<percentage<<" %\n"<<std::endl;

    //        ------------ RESTART LEAST SQUARES ------------
  }else if(_imvjRestartType == MVQNPostProcessing::RS_LS)
  {
    // drop all stored Wtil^q, Z^q matrices
   _WtilChunk.clear();
   _pseudoInverseChunk.clear();

   if(_matrixV_RSLS.cols() > 0){
     // avoid that the syste mis getting too squared
     while(_matrixV_RSLS.cols()*2 >= getLSSystemRows()){
       removeMatrixColumnRSLS(_matrixV_RSLS.cols()-1);
     }

     // preconditioning
     _preconditioner->apply(_matrixV_RSLS);
     _preconditioner->apply(_matrixW_RSLS);

     QRFactorization qr(_filter);
     qr.setGlobalRows(getLSSystemRows());
     // for QR2-filter, the QR-dec is computed in qr-applyFilter()
     if(_filter != PostProcessing::QR2FILTER){
       for(int i = 0; i < (int)_matrixV_RSLS.cols(); i++){
         Eigen::VectorXd v = _matrixV_RSLS.col(i);
         qr.pushBack(v);  // same order as matrix V_RSLS
       }
     }

     // apply filter
     if (_filter != PostProcessing::NOFILTER) {
       std::vector<int> delIndices(0);
       qr.applyFilter(_singularityLimit, delIndices, _matrixV_RSLS);
       // start with largest index (as V,W matrices are shrinked and shifted
       for (int i = delIndices.size() - 1; i >= 0; i--) {
         removeMatrixColumnRSLS(delIndices[i]);
       }
       assertion2(_matrixV_RSLS.cols() == qr.cols(), _matrixV_RSLS.cols(), qr.cols());
     }

     /**
      *   computation of pseudo inverse matrix Z = (V^TV)^-1 * V^T as solution
      *   to the equation R*z = Q^T(i) for all columns i,  via back substitution.
      */
      auto Q = qr.matrixQ();
      auto R = qr.matrixR();
      Eigen::MatrixXd pseudoInverse(qr.cols(), qr.rows());
      Eigen::VectorXd yVec(pseudoInverse.rows());

      // backsubstitution
      for (int i = 0; i < Q.rows(); i++) {
        Eigen::VectorXd Qrow = Q.row(i);
        yVec = R.triangularView<Eigen::Upper>().solve<Eigen::OnTheLeft>(Qrow);
        pseudoInverse.col(i) = yVec;
      }

      // store factorization of least-squares initial guess for Jacobian
     _WtilChunk.push_back(_matrixW_RSLS);
     _pseudoInverseChunk.push_back(pseudoInverse);

     _preconditioner->revert(_matrixW_RSLS);
     _preconditioner->revert(_matrixV_RSLS);

   }

   preciceDebug("MVJ-RESTART, mode=LS. Restart with "<<_matrixV_RSLS.cols()<<" columns from "<<_RSLSreusedTimesteps<<" time steps.");
   if (utils::MasterSlave::_masterMode || (not utils::MasterSlave::_masterMode && not utils::MasterSlave::_slaveMode))
         _infostream<<" - MVJ-RESTART" <<_nbRestarts<<", mode= LS -\n  used cols: "<<_matrixV_RSLS.cols()<<"\n  R_RS: "<<_RSLSreusedTimesteps<<"\n"<<std::endl;

   //            ------------ RESTART ZERO ------------
  }else if(_imvjRestartType == MVQNPostProcessing::RS_ZERO)
  {
    // drop all stored Wtil^q, Z^q matrices
    _WtilChunk.clear();
    _pseudoInverseChunk.clear();

    preciceDebug("MVJ-RESTART, mode=Zero");

  }else if (_imvjRestartType == MVQNPostProcessing::NO_RESTART){
    assertion(false); // should not happen, in this case _imvjRestart=false
  }else{
    assertion(false);
  }
}

// ==================================================================================
void MVQNPostProcessing:: specializedIterationsConverged
(
   DataMap & cplData)
{
  preciceTrace(__func__);

  // truncate V_RSLS and W_RSLS matrices according to _RSLSreusedTimesteps
  if(_imvjRestartType == RS_LS){
    if (_matrixCols_RSLS.front() == 0) { // Did only one iteration
      _matrixCols_RSLS.pop_front();
    }
    if (_RSLSreusedTimesteps == 0) {
      _matrixV_RSLS.resize(0,0);
      _matrixW_RSLS.resize(0,0);
      _matrixCols_RSLS.clear();
    }else if ((int) _matrixCols_RSLS.size() > _RSLSreusedTimesteps) {
      int toRemove = _matrixCols_RSLS.back();
      assertion1(toRemove > 0, toRemove);
      if(_matrixV_RSLS.size() > 0) assertion2(_matrixV_RSLS.cols() > toRemove, _matrixV_RSLS.cols(), toRemove);

      // remove columns
      for (int i = 0; i < toRemove; i++) {
        utils::removeColumnFromMatrix(_matrixV_RSLS, _matrixV_RSLS.cols() - 1);
        utils::removeColumnFromMatrix(_matrixW_RSLS, _matrixW_RSLS.cols() - 1);
      }
      _matrixCols_RSLS.pop_back();
    }
    _matrixCols_RSLS.push_front(0);
  }


  // if efficient update of imvj is enabled
  if(not _alwaysBuildJacobian || _imvjRestart)
  {
    // need to apply the preconditioner, as all data structures are reverted after
    // call to computeQNUpdate. Need to call this before the preconditioner is updated.

    // |= PRECONDITIONING  V         ============|
    _preconditioner->apply(_matrixV);

    if(_preconditioner->requireNewQR()){
      if(not (_filter==PostProcessing::QR2FILTER)){ //for QR2 filter, there is no need to do this twice
        _qrV.reset(_matrixV, getLSSystemRows());
      }
      _preconditioner->newQRfulfilled();
    }
    // |===================          ============|

    // apply the configured filter to the LS system
    // as it changed in BaseQNPostProcessing::iterationsConverged()
    BaseQNPostProcessing::applyFilter();

    //              ------- RESTART/ JACOBIAN ASSEMBLY -------
    if(_imvjRestart){

      // add the matrices Wtil and Z of the converged configuration to the storage containers
      Eigen::MatrixXd Z(_qrV.cols(), _qrV.rows());
      // compute pseudo inverse using QR factorization and back-substitution
      pseudoInverse(Z);   // TODO: re-computation could be avoided .. in this case Z needs to be preconditioned.

      // push back unscaled pseudo Inverse, Wtil is also unscaled.
      _preconditioner->apply(Z, true, false);
      // all objects in Wtil chunk and Z chunk are NOT PRECONDITIONED
      _WtilChunk.push_back(_Wtil);
      _pseudoInverseChunk.push_back(Z);

      //_info2<<std::endl;

      /**
       *  Restart the IMVJ according to restart type
       */
      if ((int)_WtilChunk.size() >= _chunkSize){

        // |= APPLY PRECONDITIONING  J_prev = Wtil^q, Z^q  ===|
        for(int i = 0; i < (int)_WtilChunk.size(); i++){
          _preconditioner->apply(_WtilChunk[i]);
          _preconditioner->revert(_pseudoInverseChunk[i], true, false);
        }
        // |===================                            ===|

        // < RESTART >
        restartIMVJ();
        _nbRestarts++;

        // |= REVERT PRECONDITIONING  J_prev = Wtil^0, Z^0  ==|
        assertion1(_WtilChunk.size() == 1, _WtilChunk.size());
        _preconditioner->revert(_WtilChunk.front());
        _preconditioner->apply(_pseudoInverseChunk.front(), true, false);
        // |===================                             ==|
      }


      // only in imvj normal mode with efficient update:
    }else{
      // |= APPLY PRECONDITIONING  W, Wtil, J    ============|
      _preconditioner->apply(_matrixW);  // only needed in buildWtil(), should not be called in buildJacobain() TODO

      // if imvj is used in no-restart mode, the full matrix J needs to be preconditioned
      // J needs to be scaled as follows:       J' := P * J * P^-1
      // where P = diag(P1,P2,...) and Pi = diag(w1i, w2i, ..).
      // Thus, P^-1 needs the weights from all procs, i.e., global weights.
      _preconditioner->apply(_oldInvJacobian,false);
      _preconditioner->revert(_oldInvJacobian,true);
      // |===================                    ============|

      // compute explicit representation of Jacobian
      buildJacobian();

      // |= REVERT PRECONDITIONING  W, Wtil, J   ============|
      _preconditioner->revert(_matrixW); // TODO: guess not needed
      _preconditioner->revert(_oldInvJacobian,false);
      _preconditioner->apply(_oldInvJacobian,true);
      // |===================                    ============|
    }
    //              ------------------------------------------

    // |= PRECONDITIONING ====                       ============|
    //_preconditioner->revert(_residuals); // TODO not needed I think ??
    _preconditioner->revert(_matrixV);

    // Note: in imvj-restart mode, type=SVD, the SVD is updated with unscaled matrices
    // Wtil^q and Z^q, hence nothing to revert in this case. type=RS-LS handles preconditioning



    /** in case of enforced initial relaxation, the matrices are cleared
     *  in case of timestepsReused > 0, the columns in _Wtil are outdated, as the Jacobian changes, hence clear
     *  in case of timestepsReused == 0 and no initial relaxation, pending deletion in performPostProcessing
     */
    if(_timestepsReused > 0 || (_timestepsReused == 0 && _forceInitialRelaxation)){
      //_Wtil.conservativeResize(0, 0);
      _resetLS = true;
    }
  }

  // only store Jacobian if imvj is in normal mode, i.e., the Jacobian is build
  if(not _imvjRestart){
    // Also need to revert the _invJacobian matrix with the preconditioner weights, as it's stored in _oldInvJacobian.
    // note: only _oldInvJacobian is reverted after computeNewtonUpdate*() or buildJacobian().
    // The Jacobian is always unscaled outside of computeNewtonUpdate*()
    _preconditioner->revert(_invJacobian,false);
    _preconditioner->apply(_invJacobian,true);

    // store inverse Jacobian from converged time step. NOT SCALED with preconditioner
    _oldInvJacobian = _invJacobian;
  }
}

// ==================================================================================
void MVQNPostProcessing:: removeMatrixColumn
(
  int columnIndex)
{
  assertion(_matrixV.cols() > 1); assertion(_Wtil.cols() > 1);


  // remove column from matrix _Wtil
  if(not _resetLS)
    utils::removeColumnFromMatrix(_Wtil, columnIndex);

  BaseQNPostProcessing::removeMatrixColumn(columnIndex);
}

// ==================================================================================
void MVQNPostProcessing:: removeMatrixColumnRSLS
(
  int columnIndex)
{
  preciceTrace2(__func__, columnIndex, _matrixV_RSLS.cols());
  assertion(_matrixV_RSLS.cols() > 1);

  utils::removeColumnFromMatrix(_matrixV_RSLS, columnIndex);
  utils::removeColumnFromMatrix(_matrixW_RSLS, columnIndex);

  // Reduce column count
  std::deque<int>::iterator iter = _matrixCols_RSLS.begin();
  int cols = 0;
  while (iter != _matrixCols_RSLS.end()) {
    cols += *iter;
    if (cols > columnIndex) {
      assertion(*iter > 0);
      *iter -= 1;
      if (*iter == 0) {
        _matrixCols_RSLS.erase(iter);
      }
      break;
    }
    iter++;
  }
}



}}} // namespace precice, cplscheme, impl

#endif
