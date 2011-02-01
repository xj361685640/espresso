#define LANGEVIN_INTEGRATOR
#define D3Q19
#define FORCES

#include <mpi.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "lbgpu.h"
#include "utils.h"
#include "parser.h"
#include "communication.h"
#include "thermostat.h"
#include "grid.h"
#include "domain_decomposition.h"
#include "integrate.h"
#include "interaction_data.h"
#include "particle_data.h"
#include "global.h"
#include "lb_boundaries_gpu.h"

#ifdef LB_GPU

float integrate_pref2 = 1.0;

/** Action number for \ref mpi_get_particles. */
#define REQ_GETPARTS  16
#ifndef D3Q19
#error The implementation only works for D3Q19 so far!
#endif

/** Struct holding the Lattice Boltzmann parameters */
LB_parameters_gpu lb_para = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 1.0 ,0.0, 0.0, 0, 0, 0, 0, 0, 0, 1, 0, {0.0, 0.0, 0.0}, 12345};

LB_values_gpu *host_values;
LB_nodes_gpu *host_nodes;
LB_particle_force *host_forces;
LB_particle *host_data;
/** Flag indicating momentum exchange between particles and fluid */
int transfer_momentum_gpu = 0;

static int max_ran = 1000000;
/*@}*/
static double tau;

/** measures the MD time since the last fluid update */
static double fluidstep = 0.0;

/** c_sound_square in LB units*/
static float c_sound_sq = 1.f/3.f;

clock_t start, end;

static FILE *datei;
static char file[300];
static int l = 0;
static int k = 10;
static void develop_output();
static void mpi_get_particles_lb(LB_particle *host_result);
static void mpi_get_particles_slave_lb();
static void mpi_send_forces_lb(LB_particle_force *host_forces);
static void mpi_send_forces_slave_lb();

int n_extern_nodeforces = 0;
LB_extern_nodeforce_gpu *host_extern_nodeforces = NULL;

/*-----------------------------------------------------------*/
/** main of lb_gpu_programm */
/*-----------------------------------------------------------*/
void lattice_boltzmann_update_gpu() {

  	fluidstep += time_step;

   
  	if (fluidstep>=tau) {

    		fluidstep=0.0;
 
        	lb_integrate_GPU();
		
			LB_TRACE (fprintf(stderr,"lb_integrate_GPU \n"));
  	}
}

/** Calculate particle lattice interactions.*/

void lb_calc_particle_lattice_ia_gpu() {

  if (transfer_momentum_gpu) {

	mpi_get_particles_lb(host_data);

	if(this_node == 0){

	LB_TRACE (for (int i=0;i<n_total_particles;i++) {
		fprintf(stderr, "%i particle posi: , %f %f %f\n", i, host_data[i].p[0], host_data[i].p[1], host_data[i].p[2]);
	})
/**----------------------------------------*/
/**Call of the particle interaction kernel */
/**----------------------------------------*/
	if (lb_para.number_of_particles) LB_particle_GPU(host_data);

	LB_TRACE (fprintf(stderr,"lb_calc_particle_lattice_ia_gpu \n"));

	}
  }
}
/**----------------------------------------*/
/**copy forces from gpu to cpu and call mpi routines to add forces to particles */
/**----------------------------------------*/
void lb_send_forces_gpu(){

  if (transfer_momentum_gpu) {

	if(this_node == 0){
		if (lb_para.number_of_particles) lb_copy_forces_GPU(host_forces);

		LB_TRACE (fprintf(stderr,"lb_send_forces_gpu \n"));

		LB_TRACE (for (int i=0;i<n_total_particles;i++) {
			fprintf(stderr, "%i particle forces , %f %f %f \n", i, host_forces[i].f[0], host_forces[i].f[1], host_forces[i].f[2]);
		})

	}
	mpi_send_forces_lb(host_forces);
  }

}
/** allocation of the needed memory for phys. values and particle data residing in the cpu memory*/
void lb_pre_init_gpu() {
	 
	lb_para.number_of_particles = n_total_particles;

	LB_TRACE (fprintf(stderr,"#particles \t %u \n", lb_para.number_of_particles));
	LB_TRACE (fprintf(stderr,"#nodes \t %u \n", lb_para.number_of_nodes));
	/**-----------------------------------------------------*/
	/** allocating of the needed memory for several structs */
	/**-----------------------------------------------------*/
	/** Struct holding the Lattice Boltzmann parameters */

	/**Struct holding calc phys values rho, j, phi of every node*/
	size_t size_of_values = lb_para.number_of_nodes * sizeof(LB_values_gpu);
	host_values = (LB_values_gpu*)malloc(size_of_values);

	/**Allocate struct for particle forces */
	size_t size_of_forces = lb_para.number_of_particles * sizeof(LB_particle_force);
	host_forces = (LB_particle_force*)malloc(size_of_forces);

	/**Allocate struct for particle positions */
	size_t size_of_positions = lb_para.number_of_particles * sizeof(LB_particle);
	host_data = (LB_particle*)malloc(size_of_positions);

	LB_TRACE (fprintf(stderr,"lb_pre_init_gpu \n"));
}
/** (re-)allocation of the memory needed for the phys. values and if needed memory for the nodes (v[19] etc.)
	located in the cpu memory*/ 
static void lb_realloc_fluid_gpu() {
	 
	LB_TRACE (printf("#nodes \t %u \n", lb_para.number_of_nodes));

	/**-----------------------------------------------------*/
	/** allocating of the needed memory for several structs */
	/**-----------------------------------------------------*/
	/** Struct holding the Lattice Boltzmann parameters */

	LB_TRACE (printf("#nodes \t %u \n", lb_para.number_of_nodes));

	/**Struct holding calc phys values rho, j, phi of every node*/
	size_t size_of_values = lb_para.number_of_nodes * sizeof(LB_values_gpu);
	host_values = realloc(host_values, size_of_values);

	LB_TRACE (fprintf(stderr,"lb_realloc_fluid_gpu \n"));
}
/** (re-) allocation of the memory need for the particles (cpu part)*/
void lb_realloc_particles_gpu(){

	lb_para.number_of_particles = n_total_particles;
	LB_TRACE (printf("#particles realloc\t %u \n", lb_para.number_of_particles));
	/**-----------------------------------------------------*/
	/** allocating of the needed memory for several structs */
	/**-----------------------------------------------------*/
	/**Allocate struct for particle forces */
	size_t size_of_forces = lb_para.number_of_particles * sizeof(LB_particle_force);
	host_forces = realloc(host_forces, size_of_forces);

	/**Allocate struct for particle positions */
	size_t size_of_positions = lb_para.number_of_particles * sizeof(LB_particle);
	host_data = realloc(host_data, size_of_positions);
	
	lb_para.your_seed = (unsigned int)i_random(max_ran);

	LB_TRACE (fprintf(stderr,"test your_seed %u \n", lb_para.your_seed));
	lb_realloc_particle_GPU(&lb_para);
}

/** (Re-)initializes the fluid according to the given value of rho. */
void lb_reinit_fluid_gpu() {

	lb_init_GPU(&lb_para);

	LB_TRACE (fprintf(stderr,"lb_reinit_fluid_gpu \n"));

}

/** Release the fluid. */
/*not needed in Espresso but still not deleted*/
void lb_release_gpu(){

    // free host memory
    free(host_nodes);
    free(host_values);
    free(host_forces);
    free(host_data);
}
/** (Re-)initializes the fluid. */
void lb_reinit_parameters_gpu() {

	lb_para.mu = 0.0;
	lb_para.time_step = (float)time_step;
	lb_para.integrate_pref2 = (float)integrate_pref2;

#ifdef LANGEVIN_INTEGRATOR
  /* force prefactor for the 2nd-order Langevin integrator */
  lb_para.integrate_pref2 = (1.-exp(-lb_para.friction*lb_para.time_step))/lb_para.friction*lb_para.time_step;
	/* one factor time_step is due to the scaled velocities */
#endif
//printf("integrate_pref2 %f \n", lb_para->integrate_pref2);	

  if (lb_para.viscosity > 0.0) {
    /* Eq. (80) Duenweg, Schiller, Ladd, PRE 76(3):036704 (2007). */
    lb_para.gamma_shear = 1. - 2./(6.*lb_para.viscosity*lb_para.tau/(lb_para.agrid*lb_para.agrid) + 1.);   
  }

  if (lb_para.bulk_viscosity > 0.0) {
    /* Eq. (81) Duenweg, Schiller, Ladd, PRE 76(3):036704 (2007). */
    lb_para.gamma_bulk = 1. - 2./(9.*lb_para.bulk_viscosity*lb_para.tau/(lb_para.agrid*lb_para.agrid) + 1.);
  }

  if (temperature > 0.0) {  /* fluctuating hydrodynamics ? */

    lb_para.fluct = 1;
	LB_TRACE (fprintf(stderr, "fluct ein \n"));
    /* Eq. (51) Duenweg, Schiller, Ladd, PRE 76(3):036704 (2007).*/
    /* Note that the modes are not normalized as in the paper here! */

    lb_para.mu = (float)temperature/c_sound_sq*lb_para.tau*lb_para.tau/(lb_para.agrid*lb_para.agrid);
    //lb_para->mu *= agrid*agrid*agrid;  // Marcello's conjecture

    /* lb_coupl_pref is stored in MD units (force)
     * Eq. (16) Ahlrichs and Duenweg, JCP 111(17):8225 (1999).
     * The factor 12 comes from the fact that we use random numbers
     * from -0.5 to 0.5 (equally distributed) which have variance 1/12.
     * time_step comes from the discretization.
     */
#ifdef LANGEVIN_INTEGRATOR
    float tmp = exp(-lb_para.friction*time_step);
    lb_para.lb_coupl_pref = lb_para.friction*sqrt(temperature*(1.+tmp)/(1.-tmp));
#else
    lb_para.lb_coupl_pref = sqrt(12.f*2.f*lb_para.friction*temperature/time_step);
#endif

  } else {
    /* no fluctuations at zero temperature */
    lb_para.fluct = 0;
    lb_para.lb_coupl_pref = 0.0;
  }
	LB_TRACE (fprintf(stderr,"lb_reinit_prarameters_gpu \n"));
}

/** Performs a full initialization of
 *  the Lattice Boltzmann system. All derived parameters
 *  and the fluid are reset to their default values. */
void lb_init_gpu() {
 	
	LB_TRACE (printf("#nodes cpu \t %i \n", lb_para.number_of_nodes));
	/** set parameters for transfer to gpu */
   	lb_reinit_parameters_gpu();

	lb_realloc_particles_gpu();
	
	lb_realloc_fluid_gpu();

	lb_init_GPU(&lb_para);

	LB_TRACE (fprintf(stderr,"lb_init_gpu \n"));

}

/*@}*/

/***********************************************************************/
/** \name MPI stuff */
/***********************************************************************/

#if 1
/*************** REQ_GETPARTS ************/
static void mpi_get_particles_lb(LB_particle *host_data)
{
  int n_part;
  int g, pnode;
  Cell *cell;
  int c;
  MPI_Status status;

  int i;	
  int *sizes;
  sizes = malloc(sizeof(int)*n_nodes);

  n_part = cells_get_n_particles();

  /* first collect number of particles on each node */
  MPI_Gather(&n_part, 1, MPI_INT, sizes, 1, MPI_INT, 0, MPI_COMM_WORLD);

  /* just check if the number of particles is correct */
  if(this_node > 0){
      /* call slave functions to provide the slave datas */
      mpi_get_particles_slave_lb();
  }
  else {
    /* master: fetch particle informations into 'result' */
    g = 0;
    for (pnode = 0; pnode < n_nodes; pnode++) {
  	  if (sizes[pnode] > 0) {
		if (pnode == 0) {
			for (c = 0; c < local_cells.n; c++) {
	  				Particle *part;
	  				int npart;	
					int dummy[3] = {0,0,0};
					double pos[3];
	  				cell = local_cells.cell[c];
	  				part = cell->part;
	 				npart = cell->n;
					 
				for (i=0;i<npart;i++) {
					
						memcpy(pos, part[i].r.p, 3*sizeof(double));
						fold_position(pos, dummy);

						host_data[i+g].p[0] = (float)pos[0];
						host_data[i+g].p[1] = (float)pos[1];
						host_data[i+g].p[2] = (float)pos[2];
								
						host_data[i+g].v[0] = (float)part[i].m.v[0];
						host_data[i+g].v[1] = (float)part[i].m.v[1];
						host_data[i+g].v[2] = (float)part[i].m.v[2];
				}
	  			g += npart;
			}
	  }
	    else {

			MPI_Recv(&host_data[g], sizes[pnode]*sizeof(LB_particle), MPI_BYTE, pnode, REQ_GETPARTS,
		 		MPI_COMM_WORLD, &status);
			g += sizes[pnode];
	  	}
	
	}
    }
  }


  COMM_TRACE(fprintf(stderr, "%d: finished get\n", this_node));

  free(sizes);
}

static void mpi_get_particles_slave_lb(){
 
  int n_part;
  int g;
  LB_particle *host_data_sl;
  Cell *cell;
  int c, i;

  n_part = cells_get_n_particles();

  COMM_TRACE(fprintf(stderr, "%d: get_particles_slave, %d particles\n", this_node, n_part));

  if (n_part > 0) {

    /* get (unsorted) particle informations as an array of type 'particle' */
    /* then get the particle information */
    host_data_sl = malloc(n_part*sizeof(LB_particle));
    
    g = 0;
    for (c = 0; c < local_cells.n; c++) {
      Particle *part;
      int npart;
	  int dummy[3] = {0,0,0};
	  double pos[3];
      cell = local_cells.cell[c];
      part = cell->part;
      npart = cell->n;

		for (i=0;i<npart;i++) {
				memcpy(pos, part[i].r.p, 3*sizeof(double));
				fold_position(pos, dummy);	
			
				host_data_sl[i+g].p[0] = (float)pos[0];
				host_data_sl[i+g].p[1] = (float)pos[1];
				host_data_sl[i+g].p[2] = (float)pos[2];

				host_data_sl[i+g].v[0] = (float)part[i].m.v[0];
				host_data_sl[i+g].v[1] = (float)part[i].m.v[1];
				host_data_sl[i+g].v[2] = (float)part[i].m.v[2];
		}
        g+=npart;
    }
    /* and send it back to the master node */
    MPI_Send(host_data_sl, n_part*sizeof(LB_particle), MPI_BYTE, 0, REQ_GETPARTS, MPI_COMM_WORLD);
    free(host_data_sl);
  }
}

static void mpi_send_forces_lb(LB_particle_force *host_forces){
	
  int n_part;
  int g, pnode;
  Cell *cell;
  int c;


	int i;	
	int *sizes;
  	sizes = malloc(sizeof(int)*n_nodes);

  n_part = cells_get_n_particles();
	//fprintf(stderr, "%i: pnode parts %i \n", this_node, n_part);
  /* first collect number of particles on each node */
  MPI_Gather(&n_part, 1, MPI_INT, sizes, 1, MPI_INT, 0, MPI_COMM_WORLD);

  /* call slave functions to provide the slave datas */
  if(this_node > 0) {
	mpi_send_forces_slave_lb();
  }
  else{
  /* fetch particle informations into 'result' */
  g = 0;
    for (pnode = 0; pnode < n_nodes; pnode++) {
  	  if (sizes[pnode] > 0) {
		if (pnode == 0) {
			for (c = 0; c < local_cells.n; c++) {
	  				int npart;	
	  				cell = local_cells.cell[c];
	 				npart = cell->n;
 
				for (i=0;i<npart;i++) {
						cell->part[i].f.f[0] += (double)host_forces[i+g].f[0];
						cell->part[i].f.f[1] += (double)host_forces[i+g].f[1];
						cell->part[i].f.f[2] += (double)host_forces[i+g].f[2];
				}
 				g += npart;
			}
	  }
	    else {
    		/* and send it back to the slave node */
    		MPI_Send(&host_forces[g], sizes[pnode]*sizeof(LB_particle_force), MPI_BYTE, pnode, REQ_GETPARTS, MPI_COMM_WORLD);			

			g += sizes[pnode];
	  	}
	
	 }
    }
  }
  COMM_TRACE(fprintf(stderr, "%d: finished send\n", this_node));

  free(sizes);

}

static void mpi_send_forces_slave_lb(){

  int n_part;
  int g;
  LB_particle_force *host_forces_sl;
  Cell *cell;
  int c, i;
  MPI_Status status;

  n_part = cells_get_n_particles();

  COMM_TRACE(fprintf(stderr, "%d: send_particles_slave, %d particles\n", this_node, n_part));


  if (n_part > 0) {

    /* get (unsorted) particle informations as an array of type 'particle' */
    /* then get the particle information */
    host_forces_sl = malloc(n_part*sizeof(LB_particle_force));
	
	MPI_Recv(host_forces_sl, n_part*sizeof(LB_particle_force), MPI_BYTE, 0, REQ_GETPARTS,
		 		MPI_COMM_WORLD, &status);

	
			for (c = 0; c < local_cells.n; c++) {
	  				int npart;	
	  				cell = local_cells.cell[c];
	 				npart = cell->n;

				for (i=0;i<npart;i++) {
						cell->part[i].f.f[0] += (double)host_forces_sl[i+g].f[0];
						cell->part[i].f.f[1] += (double)host_forces_sl[i+g].f[1];
						cell->part[i].f.f[2] += (double)host_forces_sl[i+g].f[2];
				}
	  			g += npart;
			}
	free(host_forces_sl);
  }
}
#endif
/*@}*/

/***********************************************************************/
/** \name TCL stuff */
/***********************************************************************/


static int lbfluid_parse_tau(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double tau;

    if (argc < 1) {
	Tcl_AppendResult(interp, "tau requires 1 argument", NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(tau)) {
	Tcl_AppendResult(interp, "wrong  argument for tau", (char *)NULL);
	return TCL_ERROR;
    }
    if (tau < 0.0) {
	Tcl_AppendResult(interp, "tau must be positive", (char *)NULL);
	return TCL_ERROR;
    }
    else if ((time_step >= 0.0) && (tau < time_step)) {
		fprintf(stderr,"tau %f \n", lb_para.tau);
      Tcl_AppendResult(interp, "tau must be larger than MD time_step", (char *)NULL);
      return TCL_ERROR;
    }

    *change = 1;
    lb_para.tau = (float)tau;

    return TCL_OK;
}

static int lbfluid_parse_agrid(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double agrid;

    if (argc < 1) {
	Tcl_AppendResult(interp, "agrid requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(agrid)) {
	Tcl_AppendResult(interp, "wrong argument for agrid", (char *)NULL);
	return TCL_ERROR;
    }
    if (agrid <= 0.0) {
	Tcl_AppendResult(interp, "agrid must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lb_para.agrid = (float)agrid;

    lb_para.dim_x = (unsigned int)floor(box_l[0]/agrid);
    lb_para.dim_y = (unsigned int)floor(box_l[1]/agrid);
    lb_para.dim_z = (unsigned int)floor(box_l[2]/agrid);

    unsigned int tmp[3];
    tmp[0] = lb_para.dim_x;
    tmp[1] = lb_para.dim_y;
    tmp[2] = lb_para.dim_z;
  /* sanity checks */
    int dir;
  for (dir=0;dir<3;dir++) {
    /* check if box_l is compatible with lattice spacing */
    if (fabs(box_l[dir]-tmp[dir]*agrid) > ROUND_ERROR_PREC) {
      char *errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt, "{097 Lattice spacing agrid=%f is incompatible with box_l[%i]=%f} ",agrid,dir,box_l[dir]);
    }
  }

	lb_para.number_of_nodes = lb_para.dim_x * lb_para.dim_y * lb_para.dim_z;
	LB_TRACE (printf("#nodes \t %u \n", lb_para.number_of_nodes));
 
    return TCL_OK;
}

static int lbfluid_parse_density(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double density;

    if (argc < 1) {
	Tcl_AppendResult(interp, "density requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(density)) {
	Tcl_AppendResult(interp, "wrong argument for density", (char *)NULL);
	return TCL_ERROR;
    }
    if (density <= 0.0) {
	Tcl_AppendResult(interp, "density must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lb_para.rho = (float)density;

    return TCL_OK;
}

static int lbfluid_parse_viscosity(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double viscosity;

    if (argc < 1) {
	Tcl_AppendResult(interp, "viscosity requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(viscosity)) {
	Tcl_AppendResult(interp, "wrong argument for viscosity", (char *)NULL);
	return TCL_ERROR;
    }
    if (viscosity <= 0.0) {
	Tcl_AppendResult(interp, "viscosity must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lb_para.viscosity = (float)viscosity;
 
    return TCL_OK;
}

static int lbfluid_parse_bulk_visc(Tcl_Interp *interp, int argc, char *argv[], int *change) {
  double bulk_visc;

  if (argc < 1) {
    Tcl_AppendResult(interp, "bulk_viscosity requires 1 argument", (char *)NULL);
    return TCL_ERROR;
  }
  if (!ARG0_IS_D(bulk_visc)) {
    Tcl_AppendResult(interp, "wrong argument for bulk_viscosity", (char *)NULL);
    return TCL_ERROR;
  }
  if (bulk_visc < 0.0) {
    Tcl_AppendResult(interp, "bulk_viscosity must be positive", (char *)NULL);
    return TCL_ERROR;
  }

  *change =1;
  lb_para.bulk_viscosity = (float)bulk_visc;

  return TCL_OK;

}

static int lbfluid_parse_friction(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double friction;

    if (argc < 1) {
	Tcl_AppendResult(interp, "friction requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(friction)) {
	Tcl_AppendResult(interp, "wrong argument for friction", (char *)NULL);
	return TCL_ERROR;
    }
    if (friction <= 0.0) {
	Tcl_AppendResult(interp, "friction must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lb_para.friction = (float)friction;

    return TCL_OK;
}

static int lbfluid_parse_ext_force(Tcl_Interp *interp, int argc, char *argv[], int *change) {

    double ext_f[3];
    if (argc < 3) {
	Tcl_AppendResult(interp, "ext_force requires 3 arguments", (char *)NULL);
	return TCL_ERROR;
    }
    else {
 	if (!ARG_IS_D(0, ext_f[0])) return TCL_ERROR;
	if (!ARG_IS_D(1, ext_f[1])) return TCL_ERROR;
	if (!ARG_IS_D(2, ext_f[2])) return TCL_ERROR;
    }
    
    *change = 3;

    /* external force density is stored in MD units */
    lb_para.ext_force[0] = (float)ext_f[0];
    lb_para.ext_force[1] = (float)ext_f[1];
    lb_para.ext_force[2] = (float)ext_f[2];

	lb_para.external_force = 1;
    
    return TCL_OK;
}

static int lbfluid_parse_gamma_odd(Tcl_Interp *interp, int argc, char *argv[], int *change) {

    double g;

    if (argc < 1) {
	Tcl_AppendResult(interp, "gamma_odd requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(g)) {
	Tcl_AppendResult(interp, "wrong argument for gamma_odd", (char *)NULL);
	return TCL_ERROR;
    }
    if (fabs( g > 1.0)) {
	Tcl_AppendResult(interp, "fabs(gamma_odd) must be > 1.", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lb_para.gamma_odd = (float)g;

    return TCL_OK;
}

static int lbfluid_parse_gamma_even(Tcl_Interp *interp, int argc, char *argv[], int *change) {

    double g;

    if (argc < 1) {
	Tcl_AppendResult(interp, "gamma_even requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(g)) {
	Tcl_AppendResult(interp, "wrong argument for gamma_even", (char *)NULL);
	return TCL_ERROR;
    }
    if (fabs( g > 1.0)) {
	Tcl_AppendResult(interp, "fabs(gamma_even) must be > 1.", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lb_para.gamma_even = (float)g;

    return TCL_OK;
}

static int lbprint_parse_velocity(Tcl_Interp *interp, int argc, char *argv[], int *change) {

    if (argc < 1) {
	Tcl_AppendResult(interp, "file requires at least 1 argument", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;

	datei=fopen(argv[0],"w");
		if(datei == NULL){
			fprintf(stderr, "couldn't open datafile! \n");
			exit(1);
		}
	lb_get_values_GPU(host_values);
	fprintf(datei, "# vtk DataFile Version 2.0\ntest\nASCII\nDATASET STRUCTURED_POINTS\nDIMENSIONS %u %u %u\nORIGIN 0 0 0\nSPACING 1 1 1\nPOINT_DATA %u\nSCALARS OutArray  floats 3\nLOOKUP_TABLE default\n", lb_para.dim_x, lb_para.dim_y, lb_para.dim_z, lb_para.number_of_nodes);
	int j;	
	for(j=0; j<lb_para.number_of_nodes; ++j){
	/** print of the calculated phys values */
		fprintf(datei, " %f \t %f \t %f \n", host_values[j].v[0], host_values[j].v[1], host_values[j].v[2]);

	}

    return TCL_OK;
}
static int lbprint_parse_density(Tcl_Interp *interp, int argc, char *argv[], int *change) {

    if (argc < 1) {
	Tcl_AppendResult(interp, "file requires at least 1 argument", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;

	datei=fopen(argv[0],"w");
		if(datei == NULL){
			fprintf(stderr, "couldn't open datafile! \n");
			exit(1);
		}
	lb_get_values_GPU(host_values);
	int j;	
	for(j=0; j<lb_para.number_of_nodes; ++j){
	/** print of the calculated phys values */
		fprintf(datei, " %f \n", host_values[j].rho);
	}

    return TCL_OK;
}
static int lbprint_parse_stresstensor(Tcl_Interp *interp, int argc, char *argv[], int *change) {
#if 0
    if (argc < 1) {
	Tcl_AppendResult(interp, "file requires at least 1 argument", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;

	datei=fopen(argv[0],"w");
		if(datei == NULL){
			fprintf(stderr, "couldn't open datafile! \n");
			exit(1);
		}
	lb_get_values_GPU(host_values);
	int j;	
	for(j=0; j<lb_para.number_of_nodes; ++j){
	/** print of the calculated phys values */
		fprintf(datei, " %f \t %f \t %f \t %f \t %f \t %f \n", host_values[j].pi[0], host_values[j].pi[1], host_values[j].pi[2],
 															   host_values[j].pi[3], host_values[j].pi[4], host_values[j].pi[5]);
	}

    return TCL_OK;
#endif

}
#endif /* LB_GPU */

/** Parser for the \ref lbnode command. */
#ifdef LB_GPU
int tclcommand_lbnode_gpu(ClientData data, Tcl_Interp *interp, int argc, char **argv) {

#if 0 
   int err=TCL_ERROR;
   int coord[3];

   --argc; ++argv;
   int i;
 
   if (lbfluid[0][0]==0) {
     Tcl_AppendResult(interp, "lbnode: lbfluid not correctly initialized", (char *)NULL);
     return TCL_ERROR;
   }

   
   if (argc < 3) {
     Tcl_AppendResult(interp, "too few arguments for lbnode", (char *)NULL);
     return TCL_ERROR;
   }

   if (!ARG_IS_I(0,coord[0]) || !ARG_IS_I(1,coord[1]) || !ARG_IS_I(2,coord[2])) {
     Tcl_AppendResult(interp, "wrong arguments for lbnode", (char *)NULL);
     return TCL_ERROR;
   } 
   argc-=3; argv+=3;

   if (argc == 0 ) { 
     Tcl_AppendResult(interp, "lbnode syntax: lbnode X Y Z [ print | set ] [ rho | u | pi | pi_neq | boundary | populations ]", (char *)NULL);
     return TCL_ERROR;
   }
   if (ARG0_IS_S("print"))
     err = lbnode_parse_print(interp, argc-1, argv+1, coord);
   else if (ARG0_IS_S("set")) 
     err = lbnode_parse_set(interp, argc-1, argv+1, coord);
   else {
     Tcl_AppendResult(interp, "unknown feature \"", argv[0], "\" of lbnode", (char *)NULL);
     return  TCL_ERROR;
   }     
   return err;
#endif

Tcl_AppendResult(interp, "lbnode_gpu command currently not implemented in the GPU code, please use the CPU code", (char *)NULL);
     return TCL_ERROR;

//#else /* !defined LB_GPU */
//  Tcl_AppendResult(interp, "LB_GPU is not compiled in!", NULL);
//  return TCL_ERROR;

}
#endif /* LB_GPU */
static int lbnode_parse_set(Tcl_Interp *interp, int argc, char **argv, int *ind) {
  unsigned int index;
  double f[3];
  size_t size_of_extforces;
  int change = 0;

  if ( ind[0] >=  lb_para.dim_x ||  ind[1] >= lb_para.dim_y ||  ind[2] >= lb_para.dim_z ) {
      Tcl_AppendResult(interp, "position is not in the LB lattice", (char *)NULL);
    return TCL_ERROR;
  }

  index = ind[0] + ind[1]*lb_para.dim_x + ind[2]*lb_para.dim_x*lb_para.dim_y;
  while (argc > 0) {
    if (change==1) {
      Tcl_ResetResult(interp);
      Tcl_AppendResult(interp, "Error in lbnode_extforce force. You can only change one field at the same time.", (char *)NULL);
      return TCL_ERROR;
    }
	if ((ARG0_IS_S("force"))){
      if (ARG1_IS_D(f[0])) {
        argc--;
        argv++;
      } else return TCL_ERROR;
      if (ARG1_IS_D(f[1])) { 
        argc--;
        argv++;
      } else return TCL_ERROR;
      if (ARG1_IS_D(f[2])) {
        argc--;
        argv++;
      } else return TCL_ERROR;
      change=1;
	}

	size_of_extforces = (n_extern_nodeforces+1)*sizeof(LB_extern_nodeforce_gpu);
	host_extern_nodeforces = realloc(host_extern_nodeforces, size_of_extforces);
 
	host_extern_nodeforces[n_extern_nodeforces].force[0] = (float)f[0];
	host_extern_nodeforces[n_extern_nodeforces].force[1] = (float)f[1];
	host_extern_nodeforces[n_extern_nodeforces].force[2] = (float)f[2];

	host_extern_nodeforces[n_extern_nodeforces].index = index;
	n_extern_nodeforces++;
	  
	  if(lb_para.external_force == 0)lb_para.external_force = 1;

    --argc; ++argv;

	lb_init_extern_nodeforces_GPU(n_extern_nodeforces, host_extern_nodeforces, &lb_para);
  }

  return TCL_OK;
}

/** Parser for the \ref lbnode_extforce command. Can be used in future to set more values like rho,u e.g. */
#ifdef LB_GPU
int tclcommand_lbnode_extforce_gpu(ClientData data, Tcl_Interp *interp, int argc, char **argv) {

#if 1 
   int err=TCL_ERROR;
   int coord[3];

   --argc; ++argv;
  
   if (argc < 3) {
     Tcl_AppendResult(interp, "too few arguments for lbnode_extforce", (char *)NULL);
     return TCL_ERROR;
   }

   if (!ARG_IS_I(0,coord[0]) || !ARG_IS_I(1,coord[1]) || !ARG_IS_I(2,coord[2])) {
     Tcl_AppendResult(interp, "wrong arguments for lbnode", (char *)NULL);
     return TCL_ERROR;
   } 
   argc-=3; argv+=3;

   if (argc == 0 ) { 
     Tcl_AppendResult(interp, "lbnode_extforce syntax: lbnode_extforce X Y Z [ print | set ] [ F(X) | F(Y) | F(Z) ]", (char *)NULL);
     return TCL_ERROR;
   }

 if (ARG0_IS_S("set")) 
     err = lbnode_parse_set(interp, argc-1, argv+1, coord);
   else {
     Tcl_AppendResult(interp, "unknown feature \"", argv[0], "\" of lbnode_extforce", (char *)NULL);
     return  TCL_ERROR;
   }     
   return err;
#endif

}
#endif /* LB_GPU */
/** Parser for the \ref lbfluid command gpu. */
#ifdef LB_GPU
int tclcommand_lbfluid_gpu(ClientData data, Tcl_Interp *interp, int argc, char **argv) {

  int err = TCL_OK;
  int change = 0;

  argc--; argv++;

  if (argc < 1) {
      Tcl_AppendResult(interp, "too few arguments to \"lbfluid_gpu\"", (char *)NULL);
      err = TCL_ERROR;
  }
  else if (ARG0_IS_S("off")) {
    err = TCL_ERROR;
  }
  else if (ARG0_IS_S("init")) {
    err = TCL_ERROR;
  }
  else while (argc > 0) {
      if (ARG0_IS_S("grid") || ARG0_IS_S("agrid"))
	  err = lbfluid_parse_agrid(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("tau"))
	  err = lbfluid_parse_tau(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("density"))
	  err = lbfluid_parse_density(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("viscosity"))
	  err = lbfluid_parse_viscosity(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("bulk_viscosity"))
	  err = lbfluid_parse_bulk_visc(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("friction") || ARG0_IS_S("coupling"))
	  err = lbfluid_parse_friction(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("ext_force"))
	  err = lbfluid_parse_ext_force(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("gamma_odd"))
	  err = lbfluid_parse_gamma_odd(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("gamma_even"))
	  err = lbfluid_parse_gamma_even(interp, argc-1, argv+1, &change);
      else {
	  Tcl_AppendResult(interp, "unknown feature \"", argv[0],"\" of lbfluid", (char *)NULL);
	  err = TCL_ERROR ;
      }
      argc -= (change + 1);
      argv += (change + 1);
  }

  lattice_switch = (lattice_switch | LATTICE_LB_GPU) ;
	//fprintf(stderr,"%i lattice_switch %i \n", this_node, lattice_switch);
  mpi_bcast_parameter(FIELD_LATTICE_SWITCH) ;

  /* thermo_switch is retained for backwards compatibility */
  thermo_switch = (thermo_switch | THERMO_LB);
  mpi_bcast_parameter(FIELD_THERMO_SWITCH);
	//lb_init_gpu();

	LB_TRACE (fprintf(stderr,"tclcommand_lbfluid_gpu parser ok \n"));

  return err;
}
#endif /* LB_GPU */
#ifdef LB_GPU
int tclcommand_lbprint_gpu(ClientData data, Tcl_Interp *interp, int argc, char **argv) {

  int err = TCL_OK;
  int change = 0;

  argc--; argv++;

  if (argc < 1) {
      Tcl_AppendResult(interp, "too few arguments to \"lbprint\"", (char *)NULL);
      err = TCL_ERROR;
  }
  else while (argc > 0) {
      if (ARG0_IS_S("velocity"))
	  err = lbprint_parse_velocity(interp, argc-1, argv+1, &change); 
      else if (ARG0_IS_S("density"))
	  err = lbprint_parse_density(interp, argc-1, argv+1, &change);   
      else if (ARG0_IS_S("stresstensor")){
	  //err = lbprint_parse_stresstensor(interp, argc-1, argv+1, &change); 
	  Tcl_AppendResult(interp, "\"lbprint stresstensor\" is not available by default due to memory saving, pls ensure availablity of pi[6] (see lbgpu.h) and lbprint_parse_stresstensor()", (char *)NULL);
      err = TCL_ERROR;}  
  	  else {
	  Tcl_AppendResult(interp, "unknown feature \"", argv[0],"\" of lbprint", (char *)NULL);
	  err = TCL_ERROR ;
      }
      argc -= (change + 1);
      argv += (change + 1);

	LB_TRACE (fprintf(stderr,"tclcommand_lbprint_gpu parser ok \n"));
  }
  return err;    
}
#endif/* LB_GPU */
