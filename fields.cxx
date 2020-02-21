#include <iostream>
#include "constants.hpp"
#include "parameters.hpp"
#include "bc.hpp"
#include "matprops.hpp"
#include "utils.hpp"
#include "fields.hpp"


void allocate_variables(const Param &param, Variables& var)
{
    const int n = var.nnode;
    const int e = var.nelem;

    var.volume = new double_vec(e);
    var.volume_old = new double_vec(e);
    var.volume_n = new double_vec(n);

    var.mass = new double_vec(n);
    var.tmass = new double_vec(n);

    var.edvoldt = new double_vec(e);

    {
        // these fields are reallocated during remeshing interpolation
        var.temperature = new double_vec(n);
        var.coord0 = new array_t(n);
        var.plstrain = new double_vec(e);
        var.delta_plstrain = new double_vec(e);
        var.vel = new array_t(n, 0);
        var.strain = new tensor_t(e, 0);
        var.stress = new tensor_t(e, 0);
        var.stressyy = new double_vec(e, 0);

#ifdef RS
        var.MAX_shear = new double_vec(e);
        var.MAX_shear_0 = new double_vec(e);
        var.CL = new double_vec(e);
        var.dl_min = new double_vec(e);
        var.maxv = new double_vec(e);
        var.strain_energy = new double_vec(e);
        var.kinetic_energy = new double_vec(e);
        var.state1 = new double_vec(e);
        var.slip_velocity = new double_vec(e);
        var.friction_coefficient = new double_vec(e);
        var.RS_shear = new double_vec(e);
        var.Failure_mode = new double_vec(e);
#endif

    }

    var.ntmp= new double_vec(n);
    var.dpressure= new double_vec(e);

    var.force = new array_t(n, 0);

    var.strain_rate = new tensor_t(e, 0);

    var.shpdx = new shapefn(e);
    if (NDIMS == 3) var.shpdy = new shapefn(e);
    var.shpdz = new shapefn(e);

    var.mat = new MatProps(param, var);
}


void reallocate_variables(const Param& param, Variables& var)
{
    const int n = var.nnode;
    const int e = var.nelem;

    delete var.volume;
    delete var.volume_old;
    delete var.volume_n;
    var.volume = new double_vec(e);
    var.volume_old = new double_vec(e);
    var.volume_n = new double_vec(n);

    delete var.mass;
    delete var.tmass;
    var.mass = new double_vec(n);
    var.tmass = new double_vec(n);

    delete var.edvoldt;
    var.edvoldt = new double_vec(e);

    delete var.ntmp;
    var.ntmp = new double_vec(n);
    delete var.dpressure;
    var.dpressure = new double_vec(e);

    delete var.force;
    var.force = new array_t(n, 0);

    delete var.strain_rate;
    var.strain_rate = new tensor_t(e, 0);

    delete var.shpdx;
    delete var.shpdz;
    var.shpdx = new shapefn(e);
    if (NDIMS == 3) {
        delete var.shpdy;
        var.shpdy = new shapefn(e);
    }
    var.shpdz = new shapefn(e);

    delete var.mat;
    var.mat = new MatProps(param, var);
}


void update_temperature(const Param &param, const Variables &var,
                        double_vec &temperature, double_vec &tdot)
{
    tdot.assign(var.nnode, 0);

    class ElemFunc_temperature : public ElemFunc
    {
    private:
        const Variables &var;
        const double_vec &temperature;
        double_vec &tdot;
    public:
        ElemFunc_temperature(const Variables &var, const double_vec &temperature, double_vec &tdot) :
            var(var), temperature(temperature), tdot(tdot) {};
        void operator()(int e)
        {
            // diffusion matrix
            double D[NODES_PER_ELEM][NODES_PER_ELEM];

            const int *conn = (*var.connectivity)[e];
            double kv = var.mat->k(e) *  (*var.volume)[e]; // thermal conductivity * volumn
            const double *shpdx = (*var.shpdx)[e];
#ifdef THREED
            const double *shpdy = (*var.shpdy)[e];
#endif
            const double *shpdz = (*var.shpdz)[e];
            for (int i=0; i<NODES_PER_ELEM; ++i) {
                for (int j=0; j<NODES_PER_ELEM; ++j) {
#ifdef THREED
                    D[i][j] = (shpdx[i] * shpdx[j] +
                               shpdy[i] * shpdy[j] +
                               shpdz[i] * shpdz[j]);
#else
                    D[i][j] = (shpdx[i] * shpdx[j] +
                               shpdz[i] * shpdz[j]);
#endif
                }
            }
            for (int i=0; i<NODES_PER_ELEM; ++i) {
                double diffusion = 0;
                for (int j=0; j<NODES_PER_ELEM; ++j)
                    diffusion += D[i][j] * temperature[conn[j]];

                tdot[conn[i]] += diffusion * kv;
            }
        }
    } elemf(var, temperature, tdot);

    loop_all_elem(var.egroups, elemf);

    // Combining temperature update and bc in the same loop for efficiency,
    // since only the top boundary has Dirichlet bc, and all the other boundaries
    // have no heat flux bc.
     #pragma omp parallel for default(none)      \
         shared(var, param, tdot, temperature)
     for (int n=0; n<var.nnode; ++n) {
        if ((*var.bcflag)[n] & BOUNDZ1)
            temperature[n] = param.bc.surface_temperature;
        else
            temperature[n] -= tdot[n] * var.dt / (*var.tmass)[n];
    }
}


void update_strain_rate(const Variables& var, tensor_t& strain_rate)
{
    double *v[NODES_PER_ELEM];

    #pragma omp parallel for default(none) \
        shared(var, strain_rate) private(v)
    for (int e=0; e<var.nelem; ++e) {
        const int *conn = (*var.connectivity)[e];
        const double *shpdx = (*var.shpdx)[e];
        const double *shpdz = (*var.shpdz)[e];
        double *s = (*var.strain_rate)[e];

        for (int i=0; i<NODES_PER_ELEM; ++i)
            v[i] = (*var.vel)[conn[i]];

        // XX component
        int n = 0;
        s[n] = 0;
        for (int i=0; i<NODES_PER_ELEM; ++i)
            s[n] += v[i][0] * shpdx[i];

#ifdef THREED
        const double *shpdy = (*var.shpdy)[e];
        // YY component
        n = 1;
        s[n] = 0;
        for (int i=0; i<NODES_PER_ELEM; ++i)
            s[n] += v[i][1] * shpdy[i];
#endif

        // ZZ component
#ifdef THREED
        n = 2;
#else
        n = 1;
#endif
        s[n] = 0;
        for (int i=0; i<NODES_PER_ELEM; ++i)
            s[n] += v[i][NDIMS-1] * shpdz[i];

#ifdef THREED
        // XY component
        n = 3;
        s[n] = 0;
        for (int i=0; i<NODES_PER_ELEM; ++i)
            s[n] += 0.5 * (v[i][0] * shpdy[i] + v[i][1] * shpdx[i]);
#endif

        // XZ component
#ifdef THREED
        n = 4;
#else
        n = 2;
#endif
        s[n] = 0;
        for (int i=0; i<NODES_PER_ELEM; ++i)
            s[n] += 0.5 * (v[i][0] * shpdz[i] + v[i][NDIMS-1] * shpdx[i]);

#ifdef THREED
        // YZ component
        n = 5;
        s[n] = 0;
        for (int i=0; i<NODES_PER_ELEM; ++i)
            s[n] += 0.5 * (v[i][1] * shpdz[i] + v[i][2] * shpdy[i]);
#endif
    }
}


static void apply_damping(const Param& param, const Variables& var, array_t& force)
{
    // flatten 2d arrays to simplify indexing
    double* ff = force.data();
    const double* v = var.vel->data();
    const double small_vel = 1e-13;

    switch (param.control.damping_option) {
    case 0:
        // no damping, stress field can become very noisy
        break;
    case 1:
        // damping when force and velocity are parallel
        // acclerating when force and velocity are anti-parallel
        #pragma omp parallel for default(none)          \
            shared(var, param, ff, v)
        for (int i=0; i<var.nnode*NDIMS; ++i) {
            if (std::fabs(v[i]) > small_vel) {
                ff[i] -= param.control.damping_factor * std::copysign(ff[i], v[i]);
            }
        }
        break;
    case 2:
        // damping prop. to force
        #pragma omp parallel for default(none)          \
            shared(var, param, ff, v)
        for (int i=0; i<var.nnode*NDIMS; ++i) {
            ff[i] -= param.control.damping_factor * ff[i];
        }
        break;
    case 3:
        // damping when force and velocity are parallel
        // weakly acclerating when force and velocity are anti-parallel
        #pragma omp parallel for default(none)          \
            shared(var, param, ff, v)
        for (int i=0; i<var.nnode*NDIMS; ++i) {
            if ((ff[i]<0) == (v[i]<0)) {
                // strong damping
                ff[i] -= param.control.damping_factor * ff[i], v[i];
            }
            else {
                // weak acceleration
                ff[i] += (1 - param.control.damping_factor) * ff[i];
            }
        }
        break;
    case 4:
        // Rayleigh damping
        //
        // Velocity-dependent damping is applied when the following
        // two conditions are met:
        //  1) velocity mag. > small_vel
        //  2) |velocity damping term| > |Cundall damping term|
        // otherwise, Cundall damping is applied as in case 1.
        const double* m = &(*var.mass)[0];
        #pragma omp parallel for default(none)          \
            shared(var, param, m, ff, v)
        for (int i=0; i<var.nnode*NDIMS; ++i) {
            int n = i / NDIMS;
            double f_C = param.control.damping_factor * std::copysign(ff[i], v[i]); // This is the FLAC damping
            double f_df = f_C;
            double shearm = 0., bulkm = 0.;
            for (int j=0; j<(*var.support)[n].size(); ++j) {
                int_vec a = (*var.elemmarkers)[(*var.support)[n][j]];
                shearm += var.mat->shearm((*var.support)[n][j]);
                bulkm += var.mat->bulkm((*var.support)[n][j]);
            }
            shearm /= (*var.support)[n].size();
            bulkm /= (*var.support)[n].size();
            if (std::fabs(v[i]) > small_vel) { 
                // Rayleigh damping
                double l = 2.0; // 2 * Damping ratio / a_0
                double K = 9.0*bulkm*shearm / (3*bulkm + shearm);
                double C = l * std::sqrt(m[n] * K);
                double f_V = C * v[i]; // This is the Visc damping
                if (std::fabs(f_C) < std::fabs(f_V)) f_df = f_V;
            }
            ff[i] = f[i] - f_df;
        break;
    default:
        std::cerr << "Error: unknown damping_option: " << param.control.damping_option << '\n';
        std::exit(1);
    }
}


void update_force(const Param& param, const Variables& var, array_t& force)
{
    std::fill_n(force.data(), var.nnode*NDIMS, 0);

    class ElemFunc_force : public ElemFunc
    {
    private:
        const Variables &var;
        array_t &force;
        const double gravity;
    public:
        ElemFunc_force(const Variables &var, array_t &force, double gravity) :
            var(var), force(force), gravity(gravity) {};
        void operator()(int e)
        {
            const int *conn = (*var.connectivity)[e];
            const double *shpdx = (*var.shpdx)[e];
#ifdef THREED
            const double *shpdy = (*var.shpdy)[e];
#endif
            const double *shpdz = (*var.shpdz)[e];
            double *s = (*var.stress)[e];
            double vol = (*var.volume)[e];

            double buoy = 0;
            if (gravity != 0)
                buoy = var.mat->rho(e) * gravity / NODES_PER_ELEM;

            for (int i=0; i<NODES_PER_ELEM; ++i) {
                double *f = force[conn[i]];
#ifdef THREED
                f[0] -= (s[0]*shpdx[i] + s[3]*shpdy[i] + s[4]*shpdz[i]) * vol;
                f[1] -= (s[3]*shpdx[i] + s[1]*shpdy[i] + s[5]*shpdz[i]) * vol;
                f[2] -= (s[4]*shpdx[i] + s[5]*shpdy[i] + s[2]*shpdz[i] + buoy) * vol;
#else
                f[0] -= (s[0]*shpdx[i] + s[2]*shpdz[i]) * vol;
                f[1] -= (s[2]*shpdx[i] + s[1]*shpdz[i] + buoy) * vol;
#endif
            }
        }
    } elemf(var, force, param.control.gravity);

    loop_all_elem(var.egroups, elemf);

    apply_stress_bcs(param, var, force);

    if (param.control.is_quasi_static) {
        apply_damping(param, var, force);
    }
}


void update_velocity(const Variables& var, array_t& vel)
{
    const double* m = &(*var.mass)[0];
    // flatten 2d arrays to simplify indexing
    const double* f = var.force->data();
    double* v = vel.data();
    #pragma omp parallel for default(none) \
        shared(var, m, f, v)
    for (int i=0; i<var.nnode*NDIMS; ++i) {
        int n = i / NDIMS;
        v[i] += var.dt * f[i] / m[n];
    }
}


void update_coordinate(const Variables& var, array_t& coord)
{
    double* x = var.coord->data();
    const double* v = var.vel->data();

    #pragma omp parallel for default(none) \
        shared(var, x, v)
    for (int i=0; i<var.nnode*NDIMS; ++i) {
        x[i] += v[i] * var.dt;
    }
}


namespace {

#ifdef THREED

    void jaumann_rate_3d(double *s, double dt, double w3, double w4, double w5)
    {
        double s_inc[NSTR];

        s_inc[0] =-2.0 * s[3] * w3 - 2.0 * s[4] * w4;
        s_inc[1] = 2.0 * s[3] * w3 - 2.0 * s[5] * w5;
        s_inc[2] = 2.0 * s[4] * w4 + 2.0 * s[5] * w5;
        s_inc[3] = s[0] * w3 - s[1] * w3 - s[4] * w5 - s[5] * w4;
        s_inc[4] = s[0] * w4 - s[2] * w4 + s[3] * w5 - s[5] * w3;
        s_inc[5] = s[1] * w5 - s[2] * w5 + s[3] * w4 + s[4] * w3;

        for(int i=0; i<NSTR; ++i)  {
            s[i] += dt * s_inc[i];
        }
    }

#else

    void jaumann_rate_2d(double *s, double dt, double w2)
    {
        double s_inc[NSTR];

        s_inc[0] =-2.0 * s[2] * w2;
        s_inc[1] = 2.0 * s[2] * w2;
        s_inc[2] = s[0] * w2 - s[1] * w2;

        for(int i=0; i<NSTR; ++i)  {
            s[i] += dt * s_inc[i];
        }
    }

#endif

}


void rotate_stress(const Variables &var, tensor_t &stress, tensor_t &strain)
{
    // The spin rate tensor, W, and the Cauchy stress tensor, S, are
    //     [  0  w3  w4]     [s0 s3 s4]
    // W = [-w3   0  w5],  S=[s3 s1 s5].
    //     [-w4 -w5   0]     [s4 s5 s2]
    //
    // Stress (and strain) increment based on the Jaumann rate is
    // dt*(S*W-W*S). S*W-W*S is also symmetric.
    // So, following the indexing of stress tensor,
    // we get
    // sj[0] = dt * ( -2 * s3 * w3 - 2 * s4 * w4)
    // sj[1] = dt * (  2 * s3 * w3 - 2 * s5 * w5)
    // sj[2] = dt * (  2 * s4 * w4 + 2 * s5 * w5)
    // sj[3] = dt * ( s0 * w3 - s1 * w3 - s4 * w5 - s5 * w4)
    // sj[4] = dt * ( s0 * w4 - s2 * w4 + s3 * w5 - s5 * w3)
    // sj[5] = dt * ( s1 * w5 - s2 * w5 + s3 * w4 + s4 * w3)

    #pragma omp parallel for default(none) \
        shared(var, stress, strain)
    for (int e=0; e<var.nelem; ++e) {
        const int *conn = (*var.connectivity)[e];

#ifdef THREED

        double w3, w4, w5;
        {
            const double *shpdx = (*var.shpdx)[e];
            const double *shpdy = (*var.shpdy)[e];
            const double *shpdz = (*var.shpdz)[e];

            double *v[NODES_PER_ELEM];
            for (int i=0; i<NODES_PER_ELEM; ++i)
                v[i] = (*var.vel)[conn[i]];

            w3 = 0;
            for (int i=0; i<NODES_PER_ELEM; ++i)
                w3 += 0.5 * (v[i][0] * shpdy[i] - v[i][1] * shpdx[i]);

            w4 = 0;
            for (int i=0; i<NODES_PER_ELEM; ++i)
                w4 += 0.5 * (v[i][0] * shpdz[i] - v[i][NDIMS-1] * shpdx[i]);

            w5 = 0;
            for (int i=0; i<NODES_PER_ELEM; ++i)
                w5 += 0.5 * (v[i][1] * shpdz[i] - v[i][NDIMS-1] * shpdy[i]);
        }

        jaumann_rate_3d(stress[e], var.dt, w3, w4, w5);
        jaumann_rate_3d(strain[e], var.dt, w3, w4, w5);

#else

        double w2;
        {
            const double *shpdx = (*var.shpdx)[e];
            const double *shpdz = (*var.shpdz)[e];

            double *v[NODES_PER_ELEM];
            for (int i=0; i<NODES_PER_ELEM; ++i)
                v[i] = (*var.vel)[conn[i]];

            w2 = 0;
            for (int i=0; i<NODES_PER_ELEM; ++i)
                w2 += 0.5 * (v[i][NDIMS-1] * shpdx[i] - v[i][0] * shpdz[i]);
        }

        jaumann_rate_2d(stress[e], var.dt, w2);
        jaumann_rate_2d(strain[e], var.dt, w2);

#endif
    }
}

