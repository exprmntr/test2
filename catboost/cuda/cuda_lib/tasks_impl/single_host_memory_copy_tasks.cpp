#include "single_host_memory_copy_tasks.h"
#include <catboost/cuda/cuda_lib/cuda_manager.h>

using namespace NCudaLib;

void TSingleHostStreamSync::AddDevice(ui32 device) {
    auto& manager = GetCudaManager();
    AddDevice(manager.State->Devices[device]);
}
