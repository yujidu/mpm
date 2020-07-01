//! Constructor
template <unsigned Tdim>
mpm::MPMExplicitTwoPhase<Tdim>::MPMExplicitTwoPhase(
    const std::shared_ptr<IO>& io)
    : mpm::MPMBase<Tdim>(io) {
  //! Logger
  console_ = spdlog::get("MPMExplicitTwoPhase");
}

//! MPM Explicit two-phase pressure smoothing
template <unsigned Tdim>
void mpm::MPMExplicitTwoPhase<Tdim>::pressure_smoothing(unsigned phase) {

  // Map pressures to nodes
  if (phase == mpm::ParticlePhase::Solid) {
    // Assign pressure to nodes
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::map_pressure_to_nodes,
                  std::placeholders::_1));
  } else if (phase == mpm::ParticlePhase::Liquid) {
    // Assign pore pressure to nodes
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::map_pore_pressure_to_nodes,
                  std::placeholders::_1));
  }

#ifdef USE_MPI
  int mpi_size = 1;

  // Get number of MPI ranks
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);

  // Run if there is more than a single MPI task
  if (mpi_size > 1) {
    // MPI all reduce nodal pressure
    mesh_->template nodal_halo_exchange<double, 1>(
        std::bind(&mpm::NodeBase<Tdim>::pressure, std::placeholders::_1, phase),
        std::bind(&mpm::NodeBase<Tdim>::assign_pressure, std::placeholders::_1,
                  phase, std::placeholders::_2));
  }
#endif

  // Smooth pressure over particles
  if (phase == mpm::ParticlePhase::Solid) {
    // Smooth pressure over particles
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::compute_pressure_smoothing,
                  std::placeholders::_1));
  } else if (phase == mpm::ParticlePhase::Liquid) {
    // Smooth pore pressure over particles
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::compute_pore_pressure_smoothing,
                  std::placeholders::_1));
  }
}

//! MPM Explicit two-phase compute stress strain
template <unsigned Tdim>
void mpm::MPMExplicitTwoPhase<Tdim>::compute_stress_strain() {
  // Iterate over each particle to calculate strain
  mesh_->iterate_over_particles(std::bind(
      &mpm::ParticleBase<Tdim>::compute_strain, std::placeholders::_1, dt_));

  // Iterate over each particle to update particle volume
  mesh_->iterate_over_particles(std::bind(
      &mpm::ParticleBase<Tdim>::update_volume, std::placeholders::_1));

  // Iterate over each particle to update particle porosity
  mesh_->iterate_over_particles(std::bind(
      &mpm::ParticleBase<Tdim>::update_porosity, std::placeholders::_1, dt_));

  // Pressure smoothing
  if (pressure_smoothing_) this->pressure_smoothing(mpm::ParticlePhase::Solid);

  // Iterate over each particle to compute stress
  mesh_->iterate_over_particles(std::bind(
      &mpm::ParticleBase<Tdim>::compute_stress, std::placeholders::_1));

  // Iterate over each particle to compute pore pressure
  mesh_->iterate_over_particles(
      std::bind(&mpm::ParticleBase<Tdim>::compute_pore_pressure,
                std::placeholders::_1, dt_));

  // Pore pressure smoothing
  if (pore_pressure_smoothing_)
    this->pressure_smoothing(mpm::ParticlePhase::Liquid);
}

//! MPM Explicit two-phase solver
template <unsigned Tdim>
bool mpm::MPMExplicitTwoPhase<Tdim>::solve() {
  bool status = true;

  console_->info("MPM analysis type {}", io_->analysis_type());

  // Initialise MPI rank and size
  int mpi_rank = 0;
  int mpi_size = 1;

#ifdef USE_MPI
  // Get MPI rank
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
  // Get number of MPI ranks
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
#endif

  // Test if checkpoint resume is needed
  bool resume = false;
  if (analysis_.find("resume") != analysis_.end())
    resume = analysis_["resume"]["resume"].template get<bool>();

  // Pressure smoothing
  if (analysis_.find("pressure_smoothing") != analysis_.end())
    pressure_smoothing_ = analysis_["pressure_smoothing"].template get<bool>();

  // Pore pressure smoothing
  if (analysis_.find("pore_pressure_smoothing") != analysis_.end())
    pore_pressure_smoothing_ =
        analysis_["pore_pressure_smoothing"].template get<bool>();

  // Initialise materials
  bool mat_status = this->initialise_materials();
  if (!mat_status) {
    status = false;
    throw std::runtime_error("Initialisation of materials failed");
  }

  // Initialise mesh
  bool mesh_status = this->initialise_mesh();
  if (!mesh_status) {
    status = false;
    throw std::runtime_error("Initialisation of mesh failed");
  }

  // Initialise particles
  bool particle_status = this->initialise_particles();
  if (!particle_status) {
    status = false;
    throw std::runtime_error("Initialisation of particles failed");
  }

  // Initialise loading conditions
  bool loading_status = this->initialise_loads();
  if (!loading_status) {
    status = false;
    throw std::runtime_error("Initialisation of loads failed");
  }

  // Compute mass for each phase
  mesh_->iterate_over_particles(
      std::bind(&mpm::ParticleBase<Tdim>::compute_mass, std::placeholders::_1));

  // Check point resume
  if (resume) this->checkpoint_resume();

  // Domain decompose
  bool initial_step = (resume == true) ? false : true;
  this->mpi_domain_decompose(initial_step);

  auto solver_begin = std::chrono::steady_clock::now();
  // Main loop
  for (; step_ < nsteps_; ++step_) {

    if (mpi_rank == 0) console_->info("Step: {} of {}.\n", step_, nsteps_);

#ifdef USE_MPI
#ifdef USE_GRAPH_PARTITIONING
    // Run load balancer at a specified frequency
    if (step_ % nload_balance_steps_ == 0 && step_ != 0)
      this->mpi_domain_decompose(false);
#endif
#endif

    // Create a TBB task group
    tbb::task_group task_group;

    // Spawn a task for initialising nodes and cells
    task_group.run([&] {
      // Initialise nodes
      mesh_->iterate_over_nodes(
          std::bind(&mpm::NodeBase<Tdim>::initialise, std::placeholders::_1));

      mesh_->iterate_over_cells(
          std::bind(&mpm::Cell<Tdim>::activate_nodes, std::placeholders::_1));
    });

    // Spawn a task for particles
    task_group.run([&] {
      // Iterate over each particle to compute shapefn
      mesh_->iterate_over_particles(std::bind(
          &mpm::ParticleBase<Tdim>::compute_shapefn, std::placeholders::_1));
    });

    task_group.wait();

    // Assign mass and momentum to nodes
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::map_mass_momentum_to_nodes,
                  std::placeholders::_1));

#ifdef USE_MPI
    // Run if there is more than a single MPI task
    if (mpi_size > 1) {
      // MPI all reduce nodal mass for solid phase
      mesh_->template nodal_halo_exchange<double, 1>(
          std::bind(&mpm::NodeBase<Tdim>::mass, std::placeholders::_1,
                    mpm::ParticlePhase::Solid),
          std::bind(&mpm::NodeBase<Tdim>::update_mass, std::placeholders::_1,
                    false, mpm::ParticlePhase::Solid, std::placeholders::_2));
      // MPI all reduce nodal momentum for solid phase
      mesh_->template nodal_halo_exchange<Eigen::Matrix<double, Tdim, 1>, Tdim>(
          std::bind(&mpm::NodeBase<Tdim>::momentum, std::placeholders::_1,
                    mpm::ParticlePhase::Solid),
          std::bind(&mpm::NodeBase<Tdim>::update_momentum,
                    std::placeholders::_1, false, mpm::ParticlePhase::Solid,
                    std::placeholders::_2));

      // MPI all reduce nodal mass for liquid phase
      mesh_->template nodal_halo_exchange<double, 1>(
          std::bind(&mpm::NodeBase<Tdim>::mass, std::placeholders::_1,
                    mpm::ParticlePhase::Liquid),
          std::bind(&mpm::NodeBase<Tdim>::update_mass, std::placeholders::_1,
                    false, mpm::ParticlePhase::Liquid, std::placeholders::_2));
      // MPI all reduce nodal momentum for liquid phase
      mesh_->template nodal_halo_exchange<Eigen::Matrix<double, Tdim, 1>, Tdim>(
          std::bind(&mpm::NodeBase<Tdim>::momentum, std::placeholders::_1,
                    mpm::ParticlePhase::Liquid),
          std::bind(&mpm::NodeBase<Tdim>::update_momentum,
                    std::placeholders::_1, false, mpm::ParticlePhase::Liquid,
                    std::placeholders::_2));
    }
#endif

    // Compute nodal velocity at the begining of time step
    mesh_->iterate_over_nodes_predicate(
        std::bind(&mpm::NodeBase<Tdim>::compute_velocity,
                  std::placeholders::_1),
        std::bind(&mpm::NodeBase<Tdim>::status, std::placeholders::_1));

    // Update stress first
    if (this->stress_update_ == mpm::StressUpdate::USF)
      this->compute_stress_strain();

    // Spawn a task for external force
    task_group.run([&] {
      // Iterate over particles to compute nodal body force
      mesh_->iterate_over_particles(
          std::bind(&mpm::ParticleBase<Tdim>::map_body_force,
                    std::placeholders::_1, this->gravity_));

      // Apply particle traction and map to nodes
      mesh_->apply_traction_on_particles(this->step_ * this->dt_);

      // Iterate over each node to add concentrated node force to external force
      if (set_node_concentrated_force_)
        mesh_->iterate_over_nodes(
            std::bind(&mpm::NodeBase<Tdim>::apply_concentrated_force,
                      std::placeholders::_1, mpm::ParticlePhase::Solid,
                      (this->step_ * this->dt_)));
    });

    // Spawn a task for internal force
    task_group.run([&] {
      // Iterate over particles to compute nodal mixture and fluid internal
      // force
      mesh_->iterate_over_particles(std::bind(
          &mpm::ParticleBase<Tdim>::map_internal_force, std::placeholders::_1));

      // Iterate over particles to compute nodal drag force coefficient
      mesh_->iterate_over_particles(
          std::bind(&mpm::ParticleBase<Tdim>::map_drag_force_coefficient,
                    std::placeholders::_1));
    });
    task_group.wait();

#ifdef USE_MPI
    // Run if there is more than a single MPI task
    if (mpi_size > 1) {
      // MPI all reduce external force of mixture
      mesh_->template nodal_halo_exchange<Eigen::Matrix<double, Tdim, 1>, Tdim>(
          std::bind(&mpm::NodeBase<Tdim>::external_force, std::placeholders::_1,
                    mpm::ParticlePhase::Mixture),
          std::bind(&mpm::NodeBase<Tdim>::update_external_force,
                    std::placeholders::_1, false, mpm::ParticlePhase::Mixture,
                    std::placeholders::_2));
      // MPI all reduce external force of pore fluid
      mesh_->template nodal_halo_exchange<Eigen::Matrix<double, Tdim, 1>, Tdim>(
          std::bind(&mpm::NodeBase<Tdim>::external_force, std::placeholders::_1,
                    mpm::ParticlePhase::Liquid),
          std::bind(&mpm::NodeBase<Tdim>::update_external_force,
                    std::placeholders::_1, false, mpm::ParticlePhase::Liquid,
                    std::placeholders::_2));

      // MPI all reduce internal force of mixture
      mesh_->template nodal_halo_exchange<Eigen::Matrix<double, Tdim, 1>, Tdim>(
          std::bind(&mpm::NodeBase<Tdim>::internal_force, std::placeholders::_1,
                    mpm::ParticlePhase::Mixture),
          std::bind(&mpm::NodeBase<Tdim>::update_internal_force,
                    std::placeholders::_1, false, mpm::ParticlePhase::Mixture,
                    std::placeholders::_2));
      // MPI all reduce internal force of pore liquid
      mesh_->template nodal_halo_exchange<Eigen::Matrix<double, Tdim, 1>, Tdim>(
          std::bind(&mpm::NodeBase<Tdim>::internal_force, std::placeholders::_1,
                    mpm::ParticlePhase::Liquid),
          std::bind(&mpm::NodeBase<Tdim>::update_internal_force,
                    std::placeholders::_1, false, mpm::ParticlePhase::Liquid,
                    std::placeholders::_2));

      // MPI all reduce drag force
      mesh_->template nodal_halo_exchange<Eigen::Matrix<double, Tdim, 1>, Tdim>(
          std::bind(&mpm::NodeBase<Tdim>::drag_force_coefficient,
                    std::placeholders::_1),
          std::bind(&mpm::NodeBase<Tdim>::update_drag_force_coefficient,
                    std::placeholders::_1, false, std::placeholders::_2));
    }
#endif

    // Check if damping has been specified and accordingly Iterate over
    // active nodes to compute acceleratation and velocity
    if (damping_type_ == mpm::Damping::Cundall)
      mesh_->iterate_over_nodes_predicate(
          std::bind(&mpm::NodeBase<Tdim>::
                        compute_acceleration_velocity_twophase_explicit_cundall,
                    std::placeholders::_1, this->dt_, damping_factor_),
          std::bind(&mpm::NodeBase<Tdim>::status, std::placeholders::_1));
    else
      mesh_->iterate_over_nodes_predicate(
          std::bind(&mpm::NodeBase<
                        Tdim>::compute_acceleration_velocity_twophase_explicit,
                    std::placeholders::_1, this->dt_),
          std::bind(&mpm::NodeBase<Tdim>::status, std::placeholders::_1));

    // Update particle position and kinematics
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::compute_updated_position,
                  std::placeholders::_1, this->dt_, velocity_update_));

    // Apply particle velocity constraints
    mesh_->apply_particle_velocity_constraints();

    // Update Stress Last
    if (this->stress_update_ == mpm::StressUpdate::USL)
      this->compute_stress_strain();

    // Locate particles
    auto unlocatable_particles = mesh_->locate_particles_mesh();

    if (!unlocatable_particles.empty() && this->locate_particles_)
      throw std::runtime_error("Particle outside the mesh domain");
    // If unable to locate particles remove particles
    if (!unlocatable_particles.empty() && !this->locate_particles_)
      for (const auto& remove_particle : unlocatable_particles)
        mesh_->remove_particle(remove_particle);

#ifdef USE_MPI
#ifdef USE_GRAPH_PARTITIONING
    mesh_->transfer_halo_particles();
#endif
#endif

    if (step_ % output_steps_ == 0) {
      // HDF5 outputs
      this->write_hdf5(this->step_, this->nsteps_);
#ifdef USE_VTK
      // VTK outputs
      this->write_vtk(this->step_, this->nsteps_);
#endif
#ifdef USE_PARTIO
      // Partio outputs
      this->write_partio(this->step_, this->nsteps_);
#endif
    }
  }
  auto solver_end = std::chrono::steady_clock::now();
  console_->info(
      "Rank {}, ExplicitTwoPhase {} solver duration: {} ms", mpi_rank,
      (this->stress_update_ == mpm::StressUpdate::USL ? "USL" : "USF"),
      std::chrono::duration_cast<std::chrono::milliseconds>(solver_end -
                                                            solver_begin)
          .count());

  return status;
}