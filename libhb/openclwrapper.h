/* openclwrapper.h

   Copyright (c) 2003-2012 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html

   Authors: Peng Gao <peng@multicorewareinc.com> <http://www.multicorewareinc.com/>
            Li   Cao <li@multicorewareinc.com> <http://www.multicorewareinc.com/>


 */
#ifndef __OPENCL_WRAPPER_H
#define __OPENCL_WRAPPER_H
#ifdef USE_OPENCL
#include "common.h"

//support AMD opencl
#define CL_QUEUE_THREAD_HANDLE_AMD 0x403E
#define CL_MAP_WRITE_INVALIDATE_REGION (1 << 2)

typedef struct _KernelEnv
{
    cl_context context;
    cl_command_queue command_queue;
    cl_program program;
    cl_kernel kernel;
    char kernel_name[150];
    int isAMD;
}KernelEnv;

typedef struct _OpenCLEnv
{
    cl_platform_id platform;
    cl_context   context;
    cl_device_id devices;
    cl_command_queue command_queue;
}OpenCLEnv;


//user defined, this is function wrapper which is used to set the input parameters ,
//luanch kernel and copy data from GPU to CPU or CPU to GPU.
typedef int (*cl_kernel_function)( void **userdata, KernelEnv *kenv );

// registe a wapper for running the kernel specified by the kernel name
int hb_register_kernel_wrapper( const char *kernel_name, cl_kernel_function function );

// run kernel , user call this function to luanch kernel.
// kernel_name: this kernel name is used to find the kernel in opencl runtime environment
// userdata: this userdata is the all parameters for running the kernel specified by kernel name
int hb_run_kernel( const char *kernel_name, void **userdata );

// init the run time environment , this function must be called befor calling any function related to opencl
// the argc must be set zero , argv must be set NULL, build_option is the options for build the kernel.
int hb_init_opencl_run_env( int argc, char **argv, const char *build_option );

//relase all resource about the opencl , this function must be called after calling any functions related to opencl
int hb_release_opencl_run_env();

// get the opencl status , 0: not init ; 1, inited; this function is used the check whether or not the opencl run time has been created
int hb_opencl_stats();

// update opencl run time environments , such as commandqueue , platforme, context. program
int hb_init_opencl_attr( OpenCLEnv * env );

// create kernel object  by a kernel name on the specified opencl run time indicated by env parameter
int hb_create_kernel( char * kernelname, KernelEnv * env );

// release kernel object which is generated by calling the hb_create_kernel api
int hb_release_kernel( KernelEnv * env );

int hb_get_opencl_env();

int hb_create_buffer(cl_mem *cl_Buf,int flags,int size);

int hb_read_opencl_buffer(cl_mem cl_inBuf,unsigned char *outbuf,int size);

int hb_confirm_gpu_type();
#endif
#endif
