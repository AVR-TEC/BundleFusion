
#include "stdafx.h"
#include "CUDASolverBundling.h"
#include "../GlobalBundlingState.h"
#include "../CUDACache.h"
#include "../SiftGPU/MatrixConversion.h"

extern "C" void evalMaxResidual(SolverInput& input, SolverState& state, SolverParameters& parameters, CUDATimer* timer);
extern "C" void buildVariablesToCorrespondencesTableCUDA(EntryJ* d_correspondences, unsigned int numberOfCorrespondences, unsigned int maxNumCorrespondencesPerImage, int* d_variablesToCorrespondences, int* d_numEntriesPerRow, CUDATimer* timer);
extern "C" void solveBundlingStub(SolverInput& input, SolverState& state, SolverParameters& parameters, float* convergenceAnalysis, CUDATimer* timer);

extern "C" int countHighResiduals(SolverInput& input, SolverState& state, SolverParameters& parameters, CUDATimer* timer);

//!!!DEBUGGING
extern "C" void BuildDenseDepthSystem(SolverInput& input, SolverState& state, SolverParameters& parameters, CUDATimer* timer);
//!!!DEBUGGING

CUDASolverBundling::CUDASolverBundling(unsigned int maxNumberOfImages, unsigned int maxCorrPerImage) 
	: m_maxNumberOfImages(maxNumberOfImages), m_maxCorrPerImage(maxCorrPerImage)
, THREADS_PER_BLOCK(512) // keep consistent with the GPU
{
	m_timer = NULL;
	//if (GlobalBundlingState::get().s_enableDetailedTimings) m_timer = new CUDATimer();
	m_bRecordConvergence = GlobalBundlingState::get().s_recordSolverConvergence;

	//TODO PARAMS
	m_verifyOptDistThresh = 0.02f;//GlobalAppState::get().s_verifyOptDistThresh;
	m_verifyOptPercentThresh = 0.05f;//GlobalAppState::get().s_verifyOptPercentThresh;

	const unsigned int numberOfVariables = maxNumberOfImages;
	const unsigned int numberOfResiduums = maxNumberOfImages*maxCorrPerImage;

	// State
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_deltaRot, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_deltaTrans, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_rRot, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_rTrans, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_zRot, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_zTrans, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_pRot, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_pTrans, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_Jp, sizeof(float3)*numberOfResiduums));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_Ap_XRot, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_Ap_XTrans, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_scanAlpha, sizeof(float) * 2));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_rDotzOld, sizeof(float) *numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_precondionerRot, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_precondionerTrans, sizeof(float3)*numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_sumResidual, sizeof(float)));
	unsigned int n = (numberOfResiduums*3 + THREADS_PER_BLOCK - 1) / THREADS_PER_BLOCK;
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_maxResidual, sizeof(float) * n));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_maxResidualIndex, sizeof(int) * n));
	m_solverState.h_maxResidual = new float[n];
	m_solverState.h_maxResidualIndex = new int[n];

	MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_variablesToCorrespondences, sizeof(int)*m_maxNumberOfImages*m_maxCorrPerImage));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_numEntriesPerRow, sizeof(int)*m_maxNumberOfImages));

	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_countHighResidual, sizeof(int)));

	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_depthJtJ, sizeof(float) * 36 * numberOfVariables * numberOfVariables));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_depthJtr, sizeof(float) * 6 * numberOfVariables));

	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_corrCount, sizeof(int)));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&m_solverState.d_corrImage, sizeof(int)*160*120));
}

CUDASolverBundling::~CUDASolverBundling()
{
	if (m_timer) delete m_timer;

	// State
	MLIB_CUDA_SAFE_FREE(m_solverState.d_deltaRot);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_deltaTrans);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_rRot);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_rTrans);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_zRot);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_zTrans);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_pRot);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_pTrans);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_Jp);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_Ap_XRot);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_Ap_XTrans);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_scanAlpha);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_rDotzOld);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_precondionerRot);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_precondionerTrans);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_sumResidual);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_maxResidual);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_maxResidualIndex);
	SAFE_DELETE_ARRAY(m_solverState.h_maxResidual);
	SAFE_DELETE_ARRAY(m_solverState.h_maxResidualIndex);

	MLIB_CUDA_SAFE_FREE(d_variablesToCorrespondences);
	MLIB_CUDA_SAFE_FREE(d_numEntriesPerRow);
	
	MLIB_CUDA_SAFE_FREE(m_solverState.d_countHighResidual);
	
	MLIB_CUDA_SAFE_FREE(m_solverState.d_depthJtJ);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_depthJtr);

	MLIB_CUDA_SAFE_FREE(m_solverState.d_corrCount);
	MLIB_CUDA_SAFE_FREE(m_solverState.d_corrImage);
}

void CUDASolverBundling::solve(EntryJ* d_correspondences, unsigned int numberOfCorrespondences, unsigned int numberOfImages,
	unsigned int nNonLinearIterations, unsigned int nLinearIterations, 
	CUDACache* cudaCache, float sparseWeight, float denseWeight, float denseWeightLinFactor,
	float3* d_rotationAnglesUnknowns, float3* d_translationUnknowns,
	bool rebuildJT, bool findMaxResidual)
{
	MLIB_ASSERT(numberOfImages > 1);
	bool useSparse = sparseWeight > 0;

	float* convergence = NULL;
	if (m_bRecordConvergence) {
		m_convergence.resize(nNonLinearIterations + 1, -1.0f);
		convergence = m_convergence.data();
	}

	m_solverState.d_xRot = d_rotationAnglesUnknowns;
	m_solverState.d_xTrans = d_translationUnknowns;

	SolverParameters parameters;
	parameters.nNonLinearIterations = nNonLinearIterations;
	parameters.nLinIterations = nLinearIterations;
	parameters.verifyOptDistThresh = m_verifyOptDistThresh;
	parameters.verifyOptPercentThresh = m_verifyOptPercentThresh;

	parameters.weightSparse = sparseWeight;
	parameters.weightDenseDepthInit = denseWeight;
	parameters.weightDenseDepthLinFactor = denseWeightLinFactor;
	parameters.denseDepthDistThresh = 0.15f; //TODO params
	parameters.denseDepthNormalThresh = 0.97f;
	parameters.denseDepthMin = 0.1f;
	parameters.denseDepthMax = 3.5f;

	SolverInput solverInput;
	solverInput.d_correspondences = d_correspondences;
	solverInput.d_variablesToCorrespondences = d_variablesToCorrespondences;
	solverInput.d_numEntriesPerRow = d_numEntriesPerRow;
	solverInput.numberOfImages = numberOfImages;
	solverInput.numberOfCorrespondences = numberOfCorrespondences;

	solverInput.maxNumberOfImages = m_maxNumberOfImages;
	solverInput.maxCorrPerImage = m_maxCorrPerImage;

	solverInput.d_depthFrames = cudaCache->getCacheFramesGPU();
	solverInput.denseDepthWidth = 160; //TODO params - constant buffer?
	solverInput.denseDepthHeight = 120;
	solverInput.depthIntrinsics = MatrixConversion::toCUDA(cudaCache->getIntrinsics());

	//!!!DEBUGGING
	//parameters.weightDenseDepth = parameters.weightDenseDepthInit;
	//BuildDenseDepthSystem(solverInput, m_solverState, parameters, NULL);
	//std::vector<int> corrs(solverInput.denseDepthWidth*solverInput.denseDepthHeight);
	//MLIB_CUDA_SAFE_CALL(cudaMemcpy(corrs.data(), m_solverState.d_corrImage, sizeof(int)*solverInput.denseDepthWidth*solverInput.denseDepthHeight, cudaMemcpyDeviceToHost));
	//ColorImageR8G8B8 im(solverInput.denseDepthWidth, solverInput.denseDepthHeight); im.setPixels(vec3uc(0, 0, 0));
	//unsigned int count = 0;
	//for (unsigned int i = 0; i < corrs.size(); i++) {
	//	if (corrs[i] > 0) {
	//		count++;
	//		im.getPointer()[i] = vec3uc(255, 255, 255);
	//	}
	//}
	//FreeImageWrapper::saveImage("debug/corr.png", im);
	//!!!DEBUGGING

	if (rebuildJT) {
		buildVariablesToCorrespondencesTable(d_correspondences, numberOfCorrespondences);
	}
	solveBundlingStub(solverInput, m_solverState, parameters, convergence, m_timer);

	if (findMaxResidual) {
		computeMaxResidual(solverInput, parameters);
	}
}

void CUDASolverBundling::buildVariablesToCorrespondencesTable(EntryJ* d_correspondences, unsigned int numberOfCorrespondences)
{
	cutilSafeCall(cudaMemset(d_numEntriesPerRow, 0, sizeof(int)*m_maxNumberOfImages));

	if (numberOfCorrespondences > 0)
		buildVariablesToCorrespondencesTableCUDA(d_correspondences, numberOfCorrespondences, m_maxCorrPerImage, d_variablesToCorrespondences, d_numEntriesPerRow, m_timer);
}

void CUDASolverBundling::computeMaxResidual(SolverInput& solverInput, SolverParameters& parameters)
{
	if (parameters.weightSparse > 0.0f) {
		evalMaxResidual(solverInput, m_solverState, parameters, m_timer);
		// copy to cpu
		unsigned int n = (solverInput.numberOfCorrespondences * 3 + THREADS_PER_BLOCK - 1) / THREADS_PER_BLOCK;
		cutilSafeCall(cudaMemcpy(m_solverState.h_maxResidual, m_solverState.d_maxResidual, sizeof(float) * n, cudaMemcpyDeviceToHost));
		cutilSafeCall(cudaMemcpy(m_solverState.h_maxResidualIndex, m_solverState.d_maxResidualIndex, sizeof(int) * n, cudaMemcpyDeviceToHost));
		// compute max
		float maxResidual = 0.0f; int maxResidualIndex = 0;
		for (unsigned int i = 0; i < n; i++) {
			if (maxResidual < m_solverState.h_maxResidual[i]) {
				maxResidual = m_solverState.h_maxResidual[i];
				maxResidualIndex = m_solverState.h_maxResidualIndex[i];
			}
		}
		m_solverState.h_maxResidual[0] = maxResidual;
		m_solverState.h_maxResidualIndex[0] = maxResidualIndex;
	}
	else {
		m_solverState.h_maxResidual[0] = 0.0f;
		m_solverState.h_maxResidualIndex[0] = 0;
	}
}

bool CUDASolverBundling::getMaxResidual(EntryJ* d_correspondences, ml::vec2ui& imageIndices, float& maxRes)
{
	const float MAX_RESIDUAL = 0.05f; // nonsquared residual
	if (m_timer) m_timer->startEvent(__FUNCTION__);

	maxRes = m_solverState.h_maxResidual[0];

	// for debugging get image indices regardless
	EntryJ h_corr;
	unsigned int imIdx = m_solverState.h_maxResidualIndex[0] / 3;
	cutilSafeCall(cudaMemcpy(&h_corr, d_correspondences + imIdx, sizeof(EntryJ), cudaMemcpyDeviceToHost));
	imageIndices = ml::vec2ui(h_corr.imgIdx_i, h_corr.imgIdx_j);

	if (m_timer) m_timer->endEvent();

	if (m_solverState.h_maxResidual[0] > MAX_RESIDUAL) { // remove!
		
		return true;
	}

	return false;
}

bool CUDASolverBundling::useVerification(EntryJ* d_correspondences, unsigned int numberOfCorrespondences)
{
	SolverParameters parameters;
	parameters.nNonLinearIterations = 0;
	parameters.nLinIterations = 0;
	parameters.verifyOptDistThresh = m_verifyOptDistThresh;
	parameters.verifyOptPercentThresh = m_verifyOptPercentThresh;

	SolverInput solverInput;
	solverInput.d_correspondences = d_correspondences;
	solverInput.d_variablesToCorrespondences = NULL;
	solverInput.d_numEntriesPerRow = NULL;
	solverInput.numberOfImages = 0;
	solverInput.numberOfCorrespondences = numberOfCorrespondences;

	solverInput.maxNumberOfImages = m_maxNumberOfImages;
	solverInput.maxCorrPerImage = m_maxCorrPerImage;

	unsigned int numHighResiduals = countHighResiduals(solverInput, m_solverState, parameters, m_timer);
	unsigned int total = solverInput.numberOfCorrespondences * 3;
	//std::cout << "\t[ useVerification ] " << numHighResiduals << " / " << total << " = " << (float)numHighResiduals / total << " vs " << parameters.verifyOptPercentThresh << std::endl;
	if ((float)numHighResiduals / total >= parameters.verifyOptPercentThresh) return true;
	return false;
}
