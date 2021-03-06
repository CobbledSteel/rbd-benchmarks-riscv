#ifdef __cplusplus
extern "C" {
#endif

// Standard headers
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Argument parsing
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Julia headers (for initialization and gc commands)
#include "uv.h"
#include "julia.h"

#ifdef __cplusplus
}
#endif


#ifdef JULIA_DEFINE_FAST_TLS // only available in Julia v0.7 and above
JULIA_DEFINE_FAST_TLS()
#endif

// Declare C prototype of Julia functions
#ifdef __cplusplus
extern "C" {
#endif

jl_value_t* create_mechanism(char*, bool, int);
jl_value_t* create_state(jl_value_t*);
jl_value_t* create_dynamics_result(jl_value_t*);

void inverse_dynamics(jl_value_t*, jl_value_t*, jl_value_t*, jl_value_t*, jl_value_t*);
void mass_matrix(jl_value_t*, jl_value_t*);
void dynamics(jl_value_t*, jl_value_t*, jl_value_t*);

#ifdef __cplusplus
}
#endif

// modified from https://github.com/JuliaLang/julia/blob/8d6c1cebfd5cd97ebe33c9b84aa0f82335ed41a9/src/julia.h#L689
//#ifndef JL_GC_PUSH7
//#define JL_GC_PUSH7(arg1, arg2, arg3, arg4, arg5, arg6, arg7)                             \
//  void *__gc_stkf[] = {(void*)13, jl_pgcstack, arg1, arg2, arg3, arg4, arg5, arg6, arg7}; \
//  jl_pgcstack = (jl_gcframe_t*)__gc_stkf;
//#endif

#if SCALAR_TYPE == 1
#define T double
#endif

#if SCALAR_TYPE == 2
#define T float
#endif

int main(int argc, char *argv[])
{
    // Parse arguments.
    char *urdf = NULL;
    bool floating = false;
    char *csv = NULL;
    int c;
    opterr = 0;

    while ((c = getopt(argc, argv, "u:fc:")) != -1)
        switch (c)
        {
        case 'u':
            urdf = optarg;
            break;
        case 'f':
            floating = true;
            break;
        case 'c':
            csv = optarg;
            break;
        default:
            abort();
        }
    if (!urdf) {
        fprintf (stderr, "Must pass in URDF argument (-u).");
        abort();
    }
    if (!csv) {
        fprintf (stderr, "Must pass in CSV argument (-c).");
        abort();
    }

    int retcode;
    int i;
    uv_setup_args(argc, argv); // no-op on Windows

    // initialization
    libsupport_init();

    // FIXME: jl_options.compile_enabled = JL_OPTIONS_COMPILE_OFF;
    // JULIAC_PROGRAM_LIBNAME defined on command-line for compilation
    jl_options.image_file = JULIAC_PROGRAM_LIBNAME;
    julia_init(JL_IMAGE_JULIA_HOME);

    // GC roots.
    jl_value_t *mechanism = NULL, *state = NULL, *result = NULL;
    jl_value_t *vd_desired = NULL, *tau = NULL;
    JL_GC_PUSH5(&mechanism, &state, &result, &vd_desired, &tau);

    // Make BLAS/LAPACK single threaded.
    jl_eval_string("using RigidBodyDynamics.LinearAlgebra"); // just LinearAlgebra doesn't work for some reason.
    jl_eval_string("BLAS.set_num_threads(1)");

    // Print scalar type
    switch (SCALAR_TYPE) {
        case 1:
            printf("Scalar type: Float64\n");
            break;
        case 2:
            printf("Scalar type: Float32\n");
            break;
    }

    // Parse URDF.
    mechanism = create_mechanism(urdf, floating, SCALAR_TYPE);

    // Create MechanismState and DynamicsResult.
    state = create_state(mechanism);
    result = create_dynamics_result(mechanism);

    // Get state dimensions (possibly useful later).
    jl_eval_string("using RigidBodyDynamics");
    int nq = jl_unbox_int64(jl_call1(jl_get_function(jl_main_module, "num_positions"), state));
    int nv = jl_unbox_int64(jl_call1(jl_get_function(jl_main_module, "num_velocities"), state));

    // Get `jointwrenches` and `accelerations` work buffers/secondary outputs.
    // from result (for inverse dynamics)
    jl_value_t *jointwrenches = jl_get_field(result, "jointwrenches");
    jl_value_t *accelerations = jl_get_field(result, "accelerations");

    // Get `massmatrix` work buffer/output (for mass_matrix).
    jl_value_t *M = jl_get_field(result, "massmatrix");

    // Get `vd` (joint accelerations) work buffer/output (for dynamics)
    jl_value_t *vd = jl_get_field(result, "v??");

    // Get/create input data arrays
    jl_function_t *configuration = jl_get_function(jl_main_module, "configuration");
    jl_function_t *velocity = jl_get_function(jl_main_module, "velocity");
    jl_function_t *similar = jl_get_function(jl_main_module, "similar");
    jl_value_t* q = jl_call1(configuration, state);
    jl_value_t* v = jl_call1(velocity, state);
    vd_desired = jl_call1(similar, v); // for inverse dynamics
    tau = jl_call1(similar, v);

    // Raw arrays for setting inputs and retrieving results.
    // Note that `q`, `v`, etc. are `RigidBodyDynamics.CustomCollections.SegmentedVectors`,
    // so we have to get their parents (regular `Vector`s) first.
    // Similarly, `M` is a `Symmetric`, so get its backing array using `parent` as well.
    jl_function_t *parent = jl_get_function(jl_main_module, "parent");
    T *q_data = (T*)jl_array_data(jl_call1(parent, q));
    T *v_data = (T*)jl_array_data(jl_call1(parent, v));
    T *vd_desired_data = (T*)jl_array_data(jl_call1(parent, vd_desired));
    T *tau_data = (T*)jl_array_data(jl_call1(parent, tau));
    T *vd_data = (T*)jl_array_data(jl_call1(parent, vd));
    T *M_data = (T*)jl_array_data(jl_call1(parent, M));

    // Dummy inputs. TODO: copy from CSV data.
    for (i = 0; i < nq; i = i + 1) {
        q_data[i] = 1.0;
    }
    for (i = 0; i < nv; i = i + 1) {
        v_data[i] = 2.0;
        vd_desired_data[i] = 3.0;
        tau_data[i] = 4.0;
    }

    // Call inverse_dynamics.
    inverse_dynamics(tau, jointwrenches, accelerations, state, vd_desired);

    // Call mass_matrix.
    mass_matrix(M, state);

    // Call dynamics.
    jl_eval_string("GC.enable(false)");
    dynamics(result, state, tau);
    jl_eval_string("GC.enable(true)");

    // Pop GC roots.
    JL_GC_POP();

    // Clean up and gracefully exit.
    jl_atexit_hook(retcode);
    return 0;
}
