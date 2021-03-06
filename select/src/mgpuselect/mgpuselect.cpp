#include "../../../inc/mgpuselect.h"
#include "../../../util/cucpp.h"
#include <sstream>
#include <algorithm>

// Stop with the GPU sort after the number of remaining elements falls below
// this cutoff.
const int CutoffSize = 8192;//512;

const char* CountKernels[3] = { 
	"SelectCountUint",
	"SelectCountInt",
	"SelectCountFloat"
};
const char* HistKernels[2] = {
	"SelectHistValue",
	"SelectHistInterval"
};
const char* StreamKernels[3][3][3] = {
	{
		{
			"SelectStreamUintA",
			"SelectStreamIntA",
			"SelectStreamFloatA"
		}, {
			"SelectStreamUintAB",
			"SelectStreamIntAB",
			"SelectStreamFloatAB"
		}, {
			"SelectStreamUintInterval",
			"SelectStreamIntInterval",
			"SelectStreamFloatInterval"
		}
	}, {
		{
			"SelectStreamUintGenA",
			"SelectStreamUintGenA",
			"SelectStreamFloatGenA"
		}, {
			"SelectStreamUintGenAB",
			"SelectStreamIntGenB",
			"SelectStreamFloatGenAB"
		}, {
			"SelectStreamUintGenInterval",
			"SelectStreamIntGenInterval",
			"SelectStreamFloatGenInterval"
		}
	}, {
		{
			"SelectStreamUintPairA",
			"SelectStreamIntPairA",
			"SelectStreamFloatPairA"
		}, {
			"SelectStreamUintPairAB",
			"SelectStreamIntPairAB",
			"SelectStreamFloatPairAB"
		}, {
			"SelectStreamUintPairInterval",
			"SelectStreamIntPairInterval",
			"SelectStreamFloatPairInterval"
		}
	}
};


const char* SelectStatusStrings[] = {
	"SELECT_STATUS_SUCCESS",
	"SELECT_STATUS_NOT_INITIALIZED",
	"SELECT_STATUS_DEVICE_ALLOC_FAILED",
	"SELECT_STATUS_INVALID_CONTEXT",
	"SELECT_STATUS_KERNEL_NOT_FOUND",
	"SELECT_STATUS_KERNEL_ERROR",
	"SELECT_STATUS_LAUNCH_ERROR",
	"SELECT_STATUS_INVALID_VALUE",
	"SELECT_STATUS_DEVICE_ERROR",
	"SELECT_STATUS_NOT_IMPLEMENTED",
	"sELECT_STATUS_UNSUPPORTED_DEVICE"
};

// Returns a textual representation of the enums.
const char* SELECTAPI selectStatusString(selectStatus_t status) {
	int code = (int)status;
	if(code >= (int)(sizeof(SelectStatusStrings) / sizeof(char*))) return 0;
	return SelectStatusStrings[code];
}


struct selectEngine_d {
	ContextPtr context;
	ModulePtr module;

	// Index is type (uint, int, float)
	FunctionPtr countFuncs[3];

	// Index is single/interval.
	FunctionPtr histFuncs[2];

	FunctionPtr streamFuncs[3][3][3];

	int blockSize;
	int warpsPerBlock;
	int blocksPerSM;
	int warpsPerSM;
	int numWarps;			// total number of warps.
	int warpValues;			// min values processed per warp.
	int prevCount1;			// for caching the ranges.

	DeviceMemPtr countMem;
	DeviceMemPtr scanTotalMem;
	DeviceMemPtr scanWarpMem;
	DeviceMemPtr rangeMem1, rangeMem2;

	// Temporary arrays for holding the destation data. We cache these because
	// cuMemAlloc is incredibly slow. The user can free them with 
	// selectResetCache.
	DeviceMemPtr tempA, tempB;

	// One pair of ranges per warp.
	std::vector<int2> ranges1, ranges2;

	// Cache for global scan.
	std::vector<uint> globalScan;

	// Cache for local sort. Holds CutoffSize key/value pairs.
	std::vector<uint> sortSpaceKeys, sortSpaceVals;
};


////////////////////////////////////////////////////////////////////////////////
// Initialize the select engine.

selectStatus_t SELECTAPI selectCreateEngine(const char* kernelPath,
	selectEngine_t* engine) {

	std::auto_ptr<selectEngine_d> e(new selectEngine_d);

	CUresult result = AttachCuContext(&e->context);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_INVALID_CONTEXT;

	if(2 != e->context->Device()->ComputeCapability().first)
		return SELECT_STATUS_UNSUPPORTED_DEVICE;

	result = e->context->LoadModuleFilename(kernelPath, &e->module);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_KERNEL_NOT_FOUND;

	// These kernels run 6 blocks per SM and 4 warps per block.
	e->blockSize = 128;
	e->warpsPerBlock = e->blockSize / WarpSize;
	e->blocksPerSM = 6;
	e->warpsPerSM = e->warpsPerBlock * e->blocksPerSM;
	e->numWarps = e->warpsPerSM * e->context->Device()->NumSMs();
	e->prevCount1 = 0;

	// Reserve space for the local sort.
	e->sortSpaceKeys.resize(CutoffSize);
	e->sortSpaceVals.resize(CutoffSize);
	
	// Allow for unrolled loops of 4 values per thread.
	e->warpValues = 4 * 32;

	// Load the count kernels.
	for(int i(0); i < 3; ++i) {
		result = e->module->GetFunction(CountKernels[i], make_int3(128, 1, 1),
			&e->countFuncs[i]);
		if(CUDA_SUCCESS != result) return SELECT_STATUS_KERNEL_ERROR;
	}

	// Load the hist kernels.
	for(int i(0); i < 2; ++i) {
		result = e->module->GetFunction(HistKernels[i], make_int3(1024, 1, 1),
			&e->histFuncs[i]);
		if(CUDA_SUCCESS != result) return SELECT_STATUS_KERNEL_ERROR;
	}



	// Load the stream kernels.
	for(int i(0); i < 3; ++i) {
		result = e->module->GetFunction(StreamKernels[0][0][i],
			make_int3(128, 1, 1), &e->streamFuncs[0][0][i]);
		if(CUDA_SUCCESS != result) return SELECT_STATUS_KERNEL_ERROR;
	}



	// Allocate a range pair for each warp that can be launched.
	e->ranges1.resize(e->numWarps);
	e->ranges2.resize(e->numWarps);

	result = e->context->MemAlloc<int2>(e->numWarps, &e->rangeMem1);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_DEVICE_ALLOC_FAILED;
	
	result = e->context->MemAlloc<int2>(e->numWarps, &e->rangeMem2);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_DEVICE_ALLOC_FAILED;


	// Allocate 64 ints for counts for each warp.
	result = e->context->MemAlloc<uint>(64 * e->numWarps, &e->countMem);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_DEVICE_ALLOC_FAILED;
	
	// Allocate combined space for 
	// 1) Total counts for each bucket (64)
	// 2) Exc scan of bucket counts (64)
	// 3) k1 and k2 buckets (2)
	e->globalScan.resize(130);
	result = e->context->MemAlloc<uint>(130, &e->scanTotalMem);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_DEVICE_ALLOC_FAILED;
	
	// 3) Exc scan of k1 buckets over warp (numWarps)
	// 4) Exc scan of k2 buckets over warp (numWarps)
	// 5) Exc scan of middle buckets over warp (numWarps)
	result = e->context->MemAlloc<uint>(3 * RoundUp(e->numWarps, WarpSize),
		&e->scanWarpMem);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_DEVICE_ALLOC_FAILED;

	*engine = e.release();
	return SELECT_STATUS_SUCCESS;
}


selectStatus_t SELECTAPI selectDestroyEngine(selectEngine_t engine) {
	if(engine) delete engine;
	return SELECT_STATUS_SUCCESS;
}

selectStatus_t SELECTAPI selectCacheSize(selectEngine_t engine, size_t* size) {
	*size = 0;
	if(engine->tempA) *size = engine->tempA->Size();
	if(engine->tempB) *size += engine->tempB->Size();
	return SELECT_STATUS_SUCCESS;
}

selectStatus_t SELECTAPI selectResetCache(selectEngine_t engine) {
	engine->tempA.reset();
	engine->tempB.reset();
	return SELECT_STATUS_SUCCESS;
}


////////////////////////////////////////////////////////////////////////////////
// Finds the ranges for each block to process. Note that each range must begin
// a multiple of the block size.

int SetBlockRanges(selectEngine_t engine, int count, bool first) {
	int numWarps = engine->numWarps;
	int numBricks = DivUp(count, engine->warpValues);
	if(numBricks < numWarps) numWarps = numBricks;

	// Return if the ranges are already current.
	if(first && (count == engine->prevCount1)) return numWarps;

	std::vector<int2>& ranges = first ? engine->ranges1 : engine->ranges2;
	std::fill(ranges.begin(), ranges.end(), make_int2(0, 0));

	// Distribute the work evenly over all warps.
	div_t brickDiv = div(numBricks, numWarps);

	// Distribute the work along complete bricks.
	for(int i(0); i < numWarps; ++i) {
		int2 range;
		range.x = i ? ranges[i - 1].y : 0;
		int bricks = (i < brickDiv.rem) ? (brickDiv.quot + 1) : brickDiv.quot;
		range.y = std::min(range.x + bricks * engine->warpValues, count);
		ranges[i] = range;
	}

	CuDeviceMem* rangeMem = first ? engine->rangeMem1 : engine->rangeMem2;
	CUresult result = rangeMem->FromHost(ranges);

	if(first) engine->prevCount1 = count;
	
	return numWarps;
}


////////////////////////////////////////////////////////////////////////////////
// LocalSort

// Run a CPU-side sort over a small amount of data. This should only be done
// when the remaining sequence is very small.

void LocalSort(selectEngine_t e, selectData_t data, int k, void* key,
	void* value) {

//	printf("Local sort on %d elements.\n", data.count);

	// Grab the keys and perform an nth_elmeetn call.
	cuMemcpyDtoH(&e->sortSpaceKeys[0], data.keys, 4 * data.count);

	// If we need to resolve an index or value, copy the array.
	uint* p = &e->sortSpaceKeys[0];
	std::copy(p, p + data.count, &e->sortSpaceVals[0]);

	if(SELECT_TYPE_UINT == data.type)
		std::nth_element(p, p + k, p + data.count);
	else if(SELECT_TYPE_INT == data.type)
		std::nth_element((int*)p, (int*)p + k, (int*)p + data.count);
	else if(SELECT_TYPE_FLOAT == data.type)
		std::nth_element((float*)p, (float*)p + k, (float*)p + data.count);

	// Set the k'th smallest key.
	uint x = e->sortSpaceKeys[k];
	*((uint*)key) = x;

	// If are selecting key/value or key/index pairs, find exactly where in the
	// unsorted subset the k'th value was from.
	e->sortSpaceKeys.swap(e->sortSpaceVals);
	uint index;
	if(SELECT_CONTENT_KEYS != data.content) {
		index = 0;
		uint* unsorted = &e->sortSpaceKeys[0];
		while(unsorted[index] != x) ++index;
	}

	if(SELECT_CONTENT_INDICES == data.content)
		// Return the index. This is only used for very small arrays.
		*((uint*)value) = index;
	else if(SELECT_CONTENT_PAIRS == data.content)
		// Grab the value at index.
		cuMemcpyDtoH(value, data.values + 4 * index, 4);
}


////////////////////////////////////////////////////////////////////////////////
// selectValue

selectStatus_t selectKeyPass(selectEngine_t e, selectData_t data, int k, 
	bool first, int* count, int* offset, DeviceMemPtr& target, 
	bool* shortCircuit) {

	*shortCircuit = false;

	// Compute the number of warps to launch and set the ranges.
	CUresult result;
	int numWarps = SetBlockRanges(e, data.count, first);
	int blockCount = DivUp(numWarps, e->warpsPerBlock);

	CuDeviceMem* rangeMem = first ? e->rangeMem1 : e->rangeMem2;

	// Run the count kernel to find the histogram within each warp.
	uint mask = ((1<< data.numBits) - 1)<< data.bit;

	CuCallStack callStack;
	callStack.Push(data.keys, e->countMem, rangeMem, data.bit, data.numBits);
	result = e->countFuncs[(int)data.type]->Launch(blockCount, 1, callStack);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_LAUNCH_ERROR;

	// Run the histogram kernel to get the global totals and scans.
	callStack.Reset();
	callStack.Push(e->countMem, e->scanTotalMem, e->scanWarpMem, numWarps, k);
	result = e->histFuncs[0]->Launch(1, 1, callStack);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_LAUNCH_ERROR;

	// Retrieve the global scan.
	result = e->scanTotalMem->ToHost(e->globalScan);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_DEVICE_ERROR;

	// Retrieve the radix digit (bucket) that k maps to.
	int b1 = e->globalScan[128];
	uint digit = b1<< data.bit;

	// If all the values are in the same bucket, don't copy, just return.
	*count = e->globalScan[b1];
	if(data.count == *count) {
		*shortCircuit = true;
		return SELECT_STATUS_SUCCESS;
	}

	// Allocate space for the target copy.
	if(!target.get() || target->Size() < 4 * (uint)*count) {
		target.reset();

		// Allocate the required number of values plus 5% to avoid later 
		// resizing.
		int count2 = (int)(1.05 * *count);
		CUresult result = e->context->MemAlloc<uint>(count2, &target);
		if(CUDA_SUCCESS != result) return SELECT_STATUS_DEVICE_ALLOC_FAILED;
	}

	// Call the streaming function.
	callStack.Reset();
	callStack.Push(data.keys, rangeMem, e->scanWarpMem, target, mask, 
		b1<< data.bit, (CUdeviceptr)0);
	result = e->streamFuncs[0][0][(int)data.type]->
		Launch(blockCount, 1, callStack);
	if(CUDA_SUCCESS != result) return SELECT_STATUS_LAUNCH_ERROR;

	*offset = e->globalScan[64 + b1];

	return SELECT_STATUS_SUCCESS;
}


////////////////////////////////////////////////////////////////////////////////
// selectItem

// Returns the k'th-smallest value. If index is not null, it also returns the
// position within the original array that the value appears at.
selectStatus_t SELECTAPI selectItem(selectEngine_t e, selectData_t data,
	int k, void* key, void* value) {

	if(SELECT_CONTENT_KEYS != data.content) 
		return SELECT_STATUS_NOT_IMPLEMENTED;

	// Start at the most significant bit and work down.
	int lsb = data.bit;

	// Set data.bit to one past the most significant bit we want to select by.
	data.bit += data.numBits;

	// Use the temporary buffers in the select engine. Put the larger one in 
	// front.
	int sizeA = e->tempA ? e->tempA->Size() : 0;
	int sizeB = e->tempB ? e->tempB->Size() : 0;
	if(sizeB > sizeA) e->tempA.swap(e->tempB);
	bool first = true;

	// Loop until we've sorted all the requested bits.
	while(data.bit > lsb) {
		// Is the array small enough to break?
		if(data.count < CutoffSize) break;		

		// Find the bit range for the digit pass.
		int bit = std::max(lsb, data.bit - 6);
		data.numBits = data.bit - bit;
		data.bit = bit;

		int count, offset;
		bool shortCircuit;
		selectStatus_t status = selectKeyPass(e, data, k, first, &count, 
			&offset, e->tempA, &shortCircuit);
		
		if(SELECT_STATUS_SUCCESS != status) return status;

		// This radix pass was wasted - short circuit.
		if(shortCircuit) continue;

		// The pass compacted the array. Adjust k and repeat.
		k -= offset;
		data.keys = e->tempA->Handle();
		data.count = count;
		e->tempA.swap(e->tempB);
		first = false;
	}

	// There are two paths to this point:
	// 1) We ran out of key bits, and can assume that all the keys remaining in
	// the sequence are the same.
	// 2) The sequence was chopped down to fewer than CutoffSize elements before
	// selecting all the key bits. If this is the case, run a local sort.

	if(data.bit <= lsb) {
		// We've completed the select. Just grab the key and value out of device
		// memory.
		cuMemcpyDtoH(key, data.keys + 4 * k, 4);

		if(SELECT_CONTENT_PAIRS == data.content)
			// Grab the value at k.
			cuMemcpyDtoH(value, data.values + 4 * k, 4);
		else if(SELECT_CONTENT_INDICES == data.content)
			// The index is k. This only is possible if the entire sequence was
			// of the same key.
			*(int*)key = k;
	} else
		LocalSort(e, data, k, key, value);

	return SELECT_STATUS_SUCCESS;
}


////////////////////////////////////////////////////////////////////////////////
// selectList

// Returns a list of k-smallest values. This function performs best if the count
// of items is small or if they are clustered together. Consider using a sort
// for complicated queries.
selectStatus_t SELECTAPI selectList(selectEngine_t engine, selectData_t data,
	const int* k, int count, void* keys, void* values) {

	return SELECT_STATUS_SUCCESS;
}


////////////////////////////////////////////////////////////////////////////////
// selectInterval
// Select an interval. k1 is the first value selected and k2 is one past the 
// last value selected. The caller must provide device memory large enough to
// hold the returned interval.

selectStatus_t SELECTAPI selectInterval(selectEngine_t engine, 
	selectData_t data, int k1, int k2, CUdeviceptr keys, CUdeviceptr values) {

	return SELECT_STATUS_SUCCESS;

}

