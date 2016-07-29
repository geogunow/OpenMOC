#include "CPUSolver.h"

/**
 * @brief Constructor initializes array pointers for Tracks and Materials.
 * @details The constructor retrieves the number of energy groups and FSRs
 *          and azimuthal angles from the Geometry and TrackGenerator if
 *          passed in as parameters by the user. The constructor initalizes
 *          the number of OpenMP threads to a default of 1.
 * @param track_generator an optional pointer to the TrackGenerator
 */
CPUSolver::CPUSolver(TrackGenerator* track_generator)
  : Solver(track_generator) {

  setNumThreads(1);
  _FSR_locks = NULL;
#ifdef MPIx
  _track_message_size = 0;
  _MPI_requests = NULL;
  _MPI_sends = NULL;
  _MPI_receives = NULL;
#endif
}


/**
 * @brief Destructor deletes array for OpenMP mutual exclusion locks for
 *        FSR scalar flux updates, and calls Solver parent class destructor
 *        to deletes arrays for fluxes and sources.
 */
CPUSolver::~CPUSolver() {
  deleteMPIBuffers();
}


/**
 * @brief Returns the number of shared memory OpenMP threads in use.
 * @return the number of threads
 */
int CPUSolver::getNumThreads() {
  return _num_threads;
}


/**
 * @brief Fills an array with the scalar fluxes.
 * @details This class method is a helper routine called by the OpenMOC
 *          Python "openmoc.krylov" module for Krylov subspace methods.
 *          Although this method appears to require two arguments, in
 *          reality it only requires one due to SWIG and would be called
 *          from within Python as follows:
 *
 * @code
 *          num_fluxes = num_groups * num_FSRs
 *          fluxes = solver.getFluxes(num_fluxes)
 * @endcode
 *
 * @param fluxes an array of FSR scalar fluxes in each energy group
 * @param num_fluxes the total number of FSR flux values
 */
void CPUSolver::getFluxes(FP_PRECISION* out_fluxes, int num_fluxes) {

  if (num_fluxes != _num_groups * _num_FSRs)
    log_printf(ERROR, "Unable to get FSR scalar fluxes since there are "
               "%d groups and %d FSRs which does not match the requested "
               "%d flux values", _num_groups, _num_FSRs, num_fluxes);

  else if (_scalar_flux == NULL)
    log_printf(ERROR, "Unable to get FSR scalar fluxes since they "
               "have not yet been allocated");

  /* Copy the fluxes into the input array */
  else {
#pragma omp parallel for schedule(guided)
    for (int r=0; r < _num_FSRs; r++) {
      for (int e=0; e < _num_groups; e++)
        out_fluxes[r*_num_groups+e] = _scalar_flux(r,e);
    }
  }
}



/**
 * @brief Sets the number of shared memory OpenMP threads to use (>0).
 * @param num_threads the number of threads
 */
void CPUSolver::setNumThreads(int num_threads) {
  if (num_threads <= 0)
    log_printf(ERROR, "Unable to set the number of threads to %d "
               "since it is less than or equal to 0", num_threads);

  /* Set the number of threads for OpenMP */
  _num_threads = num_threads;
  omp_set_num_threads(_num_threads);
}


/**
 * @brief Assign a fixed source for a flat source region and energy group.
 * @details Fixed sources should be scaled to reflect the fact that OpenMOC
 *          normalizes the scalar flux such that the total energy- and
 *          volume-integrated production rate sums to 1.0.
 * @param fsr_id the flat source region ID
 * @param group the energy group
 * @param source the volume-averaged source in this group
 */
void CPUSolver::setFixedSourceByFSR(int fsr_id, int group,
                                    FP_PRECISION source) {

  Solver::setFixedSourceByFSR(fsr_id, group, source);

  /* Allocate the fixed sources array if not yet allocated */
  if (_fixed_sources == NULL) {
    int size = _num_FSRs * _num_groups;
    _fixed_sources = new FP_PRECISION[size];
    memset(_fixed_sources, 0.0, sizeof(FP_PRECISION) * size);
  }

  /* Warn the user if a fixed source has already been assigned to this FSR */
  if (_fixed_sources(fsr_id,group-1) != 0.)
    log_printf(WARNING, "Overriding fixed source %f in FSR ID=%d with %f",
               _fixed_sources(fsr_id,group-1), fsr_id, source);

  /* Store the fixed source for this FSR and energy group */
  _fixed_sources(fsr_id,group-1) = source;
}


/**
 * @brief Initializes the FSR volumes and Materials array.
 * @details This method allocates and initializes an array of OpenMP
 *          mutual exclusion locks for each FSR for use in the
 *          transport sweep algorithm.
 */
void CPUSolver::initializeFSRs() {

  Solver::initializeFSRs();

  /* Get FSR locks from TrackGenerator */
  _FSR_locks = _track_generator->getFSRLocks();
}


/**
 * @brief Allocates memory for Track boundary angular flux and leakage
 *        and FSR scalar flux arrays.
 * @details Deletes memory for old flux arrays if they were allocated
 *          for a previous simulation.
 */
void CPUSolver::initializeFluxArrays() {

  /* Delete old flux arrays if they exist */
  if (_boundary_flux != NULL)
    delete [] _boundary_flux;

  if (_start_flux != NULL)
    delete [] _start_flux;

  if (_boundary_leakage != NULL)
    delete [] _boundary_leakage;

  if (_scalar_flux != NULL)
    delete [] _scalar_flux;

  if (_old_scalar_flux != NULL)
    delete [] _old_scalar_flux;

#ifdef MPIx
  deleteMPIBuffers();
#endif

  int size;

  /* Allocate memory for the Track boundary flux and leakage arrays */
  try {
    size = 2 * _tot_num_tracks * _fluxes_per_track;

    _boundary_flux = new FP_PRECISION[size];
    _start_flux = new FP_PRECISION[size];
    _boundary_leakage = new FP_PRECISION[size];

    /* Allocate an array for the FSR scalar flux */
    size = _num_FSRs * _num_groups;
    _scalar_flux = new FP_PRECISION[size];
    _old_scalar_flux = new FP_PRECISION[size];
    memset(_scalar_flux, 0., size * sizeof(FP_PRECISION));
    memset(_old_scalar_flux, 0., size * sizeof(FP_PRECISION));

#ifdef MPIx
    /* Allocate memory for angular flux exchaning buffers */
    setupMPIBuffers();
#endif
  }
  catch (std::exception &e) {
    log_printf(ERROR, "Could not allocate memory for the fluxes");
  }
}


/**
 * @brief Allocates memory for FSR source arrays.
 * @details Deletes memory for old source arrays if they were allocated for a
 *          previous simulation.
 */
void CPUSolver::initializeSourceArrays() {

  /* Delete old sources arrays if they exist */
  if (_reduced_sources != NULL)
    delete [] _reduced_sources;

  /* Allocate memory for all source arrays */
  try {
    int size = _num_FSRs * _num_groups;
    _reduced_sources = new FP_PRECISION[size];

    /* If no fixed sources were assigned, use a zeroes array */
    if (_fixed_sources == NULL) {
      _fixed_sources = new FP_PRECISION[size];
      memset(_fixed_sources, 0.0, sizeof(FP_PRECISION) * size);
    }
  }
  catch(std::exception &e) {
    log_printf(ERROR, "Could not allocate memory for FSR sources");
  }
}


/**
 * @brief Zero each Track's boundary fluxes for each energy group
 *        and polar angle in the "forward" and "reverse" directions.
 */
void CPUSolver::zeroTrackFluxes() {

#pragma omp parallel for schedule(guided)
  for (int t=0; t < _tot_num_tracks; t++) {
    for (int d=0; d < 2; d++) {
      for (int pe=0; pe < _fluxes_per_track; pe++) {
        _boundary_flux(t, d, pe) = 0.0;
        _start_flux(t, d, pe) = 0.0;
      }
    }
  }
}


/**
 * @brief Copies values from the start flux into the boundary flux array
 *        for both the "forward" and "reverse" directions.
 */
void CPUSolver::copyBoundaryFluxes() {

#pragma omp parallel for schedule(guided)
  for (int t=0; t < _tot_num_tracks; t++) {
    for (int d=0; d < 2; d++) {
      for (int pe=0; pe < _fluxes_per_track; pe++)
        _boundary_flux(t,d,pe) = _start_flux(t, d, pe);
    }
  }
}


#ifdef MPIx
//FIXME
void CPUSolver::setupMPIBuffers() {

  /* Determine the size of the buffers */
  if (sizeof(FP_PRECISION) == 4)
    _track_message_size = _fluxes_per_track + 3;
  else
    _track_message_size = _fluxes_per_track + 2;
  int length = TRACKS_PER_BUFFER * _track_message_size;

  /* Initialize MPI requests and status */
  if (_geometry->isDomainDecomposed()) {

    if (_send_buffers.size() > 0)
      deleteMPIBuffers();

    /* Fill the hash map of send buffers */
    for (int dx=-1; dx <= 1; dx++) {
      for (int dy=-1; dy <= 1; dy++) {
        for (int dz=-1; dz <= 1; dz++) {
          if (abs(dx) + abs(dy) == 1 ||
              (dx == 0 && dy == 0 && dz != 0)) {
            int domain = _geometry->getNeighborDomain(dx, dy, dz);
            if (domain != -1) {
              FP_PRECISION* send_buffer = new FP_PRECISION[length];
              _send_buffers.push_back(send_buffer);
              FP_PRECISION* receive_buffer = new FP_PRECISION[length];
              _receive_buffers.push_back(receive_buffer);
              _neighbor_domains.push_back(domain);
            }
          }
        }
      }
    }
  }

  /* Initialize Track ID's to -1 */
  int num_domains = _neighbor_domains.size();
  for (int i=0; i < num_domains; i++) {
    int start_idx = _fluxes_per_track + 1;
    for (int idx = start_idx; idx < length; idx += _track_message_size) {
      long* track_info_location =
        reinterpret_cast<long*>(&_send_buffers.at(i)[idx]);
      track_info_location[0] = -1;
      track_info_location =
        reinterpret_cast<long*>(&_receive_buffers.at(i)[idx]);
      track_info_location[0] = -1;
    }
  }

  /* Setup MPI communication bookkeeping */
  _MPI_requests = new MPI_Request[2*num_domains];
  _MPI_sends = new bool[num_domains];
  _MPI_receives = new bool[num_domains];
  for (int i=0; i < num_domains; i++) {
    _MPI_sends[i] = false;
    _MPI_receives[i] = false;
  }
}


//FIXME
void CPUSolver::deleteMPIBuffers() {
  for (int i=0; i < _send_buffers.size(); i++) {
    delete [] _send_buffers.at(i);
  }
  _send_buffers.clear();

  for (int i=0; i < _send_buffers.size(); i++) {
    delete [] _send_buffers.at(i);
  }
  _receive_buffers.clear();
  _neighbor_domains.clear();

  delete [] _MPI_requests;
  delete [] _MPI_sends;
  delete [] _MPI_receives;
}


/**
 * @brief Prints out tracking information for cycles, traversing domain
 *        interfaces
 * @details This function prints Track starting and ending points for a cycle
 *          that traverses the entire Geometry.
 * @param track_start The starting Track ID from which the cycle is followed
 * @param domain_start The domain for the starting Track
 * @param length The number of Track's to follow across the cycle
 */
void CPUSolver::printCycle(long track_start, int domain_start, int length) {

  /* Initialize buffer for MPI communication */
  FP_PRECISION buffer[2*_fluxes_per_track];
  int message_size = sizeof(sendInfo);

  /* Initialize MPI requests and status */
  MPI_Comm MPI_cart = _geometry->getMPICart();
  int num_ranks;
  MPI_Comm_size(MPI_cart, &num_ranks);
  MPI_Status stat;
  MPI_Request request[num_ranks];

  int rank;
  MPI_Comm_rank(MPI_cart, &rank);

  /* Loop over all tracks and exchange fluxes */
  long curr_track = track_start;
  int curr_rank = domain_start;
  bool fwd = true;
  for (int t=0; t < length; t++) {

    /* Check if this rank is sending the Track */
    if (rank == curr_rank) {

      /* Get 3D Track data */
      TrackStackIndexes tsi;
      Track3D track;
      TrackGenerator3D* track_generator_3D =
        dynamic_cast<TrackGenerator3D*>(_track_generator);
      track_generator_3D->getTSIByIndex(curr_track, &tsi);
      track_generator_3D->getTrackOTF(&track, &tsi);

      /* Get connecting tracks */
      long connect;
      bool connect_fwd;
      Point* start;
      Point* end;
      int next_domain;
      if (fwd) {
        connect = track.getTrackPrdcFwd();
        connect_fwd = track.getNextFwdFwd();
        start = track.getStart();
        end = track.getEnd();
        next_domain = track.getDomainFwdOut();
      }
      else {
        connect = track.getTrackPrdcBwd();
        connect_fwd = track.getNextBwdFwd();
        start = track.getEnd();
        end = track.getStart();
        next_domain = track.getDomainBwdOut();
      }

      /* Write information */
      std::cout << rank << " " << start->getX() << " " << start->getY() << " "
        << start->getZ() << " " << end->getX() << " " << end->getY() << " "
        << end->getZ() << std::endl;

      if (fabs(end->getX() + 2) < 0.01 && fabs(end->getY() - 1) < 0.01 &&
          fabs(end->getZ() - 2) < 0.01) {
        std::cout << "GOT IT ";
        if (fwd)
          std::cout << "FWD\n";
        else
          std::cout << "BWD\n";
        exit(1);
      }

      /* Check domain for reflected boundaries */
      if (next_domain == -1) {
        next_domain = curr_rank;
        if (fwd)
          connect = track.getTrackNextFwd();
        else
          connect = track.getTrackNextBwd();
      }

      /* Pack the information */
      sendInfo si;
      si.track_id = connect;
      si.domain = next_domain;
      si.fwd = connect_fwd;

      /* Send the information */
      for (int i=0; i < num_ranks; i++)
        if (i != rank)
          MPI_Isend(&si, message_size, MPI_BYTE, i, 0, MPI_cart, &request[i]);

      /* Copy information */
      curr_rank = next_domain;
      fwd = connect_fwd;
      curr_track = connect;

      /* Wait for sends to complete */
      bool complete = false;
      while (!complete) {
        complete = true;
        for (int i=0; i < num_ranks; i++) {
          if (i != rank) {
            int flag;
            MPI_Test(&request[i], &flag, &stat);
            if (flag == 0)
              complete = false;
          }
        }
      }
    }

    /* Receiving info */
    else {

      /* Create object to receive sent information */
      sendInfo si;

      /* Issue the receive from the current node */
      MPI_Irecv(&si, message_size, MPI_BYTE, curr_rank, 0, MPI_cart,
                &request[0]);

      /* Wait for receive to complete */
      bool complete = false;
      while (!complete) {
        complete = true;
        int flag;
        MPI_Test(&request[0], &flag, &stat);
        if (flag == 0)
          complete = false;
      }

      /* Copy information */
      curr_rank = si.domain;
      fwd = si.fwd;
      curr_track = si.track_id;
    }

    MPI_Barrier(MPI_cart);
  }

  /* Join MPI at the end of communication */
  MPI_Barrier(MPI_cart);
}


//FIXME
void CPUSolver::packBuffers(std::vector<long> &packing_indexes,
                            std::vector<int> &buffer_indexes) {


  int num_domains = packing_indexes.size();
#pragma omp parallel for
  for (int i=0; i < num_domains; i++) {
    int send_domain = _neighbor_domains.at(i);
    for (long t=packing_indexes.at(i); t < _tot_num_tracks; t++) {

      /* Get 3D Track data */
      TrackStackIndexes tsi;
      Track3D track;
      TrackGenerator3D* track_generator_3D =
        dynamic_cast<TrackGenerator3D*>(_track_generator);
      track_generator_3D->getTSIByIndex(t, &tsi);
      track_generator_3D->getTrackOTF(&track, &tsi);

      /* Determine sending domains */
      if (track.getDomainFwdOut() == send_domain) {

        /* Send angular flux information */
        int idx = buffer_indexes.at(i);
        for (int pe=0; pe < _fluxes_per_track; pe++)
          _send_buffers.at(i)[idx + pe] = _boundary_flux(t,0,pe);
        idx += _fluxes_per_track;

        /* Send direction and track index */
        long track_id = track.getTrackPrdcFwd();
        _send_buffers.at(i)[idx] = 0;
        long* track_info_location =
          reinterpret_cast<long*>(&_send_buffers.at(i)[idx+1]);
        track_info_location[0] = track_id;
        buffer_indexes.at(i) += _track_message_size;
      }
      else if (track.getDomainBwdOut() == send_domain) {

        /* Send angular flux information */
        int idx = buffer_indexes.at(i);
        for (int pe=0; pe < _fluxes_per_track; pe++)
          _send_buffers.at(i)[idx + pe] = _boundary_flux(t,1,pe);
        idx += _fluxes_per_track;

        /* Send direction and track index */
        long track_id = track.getTrackPrdcBwd();
        _send_buffers.at(i)[idx] = 1;
        long* track_info_location =
          reinterpret_cast<long*>(&_send_buffers.at(i)[idx+1]);
        track_info_location[0] = track_id;
        buffer_indexes.at(i) += _track_message_size;
        //FIXME
        long t2 = track_info_location[0];
        if (track_info_location[0] == 4585) {
          std::cout << "x2 Value = " << track_info_location[0] << std::endl;
          std::cout << "Domain = " << i  << " and idx = " << idx << std::endl;
          std::cout << "BI = " << buffer_indexes.at(i) << std::endl;
          std::cout << "TMS = " << _track_message_size << std::endl;
          std::cout << "Track = " << t << std::endl;
        }
      }

      int rank;
      int index = buffer_indexes.at(i);
      MPI_Comm_rank(_geometry->getMPICart(), &rank);
      if (i == 0 && t >= 2 && rank == 1) {
        std::cout << "INDEX = " << index << std::endl;
        FP_PRECISION* buffer = _send_buffers.at(i);
        long * ti =
          reinterpret_cast<long*>(&buffer[17 + 1]);
        long t2 = ti[0];
        std::cout << t2 << std::endl;
        if (t2 != 4585) {
          std::cout << "WTF " << std::endl;
          exit(1);
        }
      }

      /* Check to see if the buffer is full */
      if (buffer_indexes.at(i) == _track_message_size * TRACKS_PER_BUFFER) {
        packing_indexes.at(i) = t+1;
        break;
      }
    }
    /* Set any remaining spots to negate the Track ID */
    int start_idx = buffer_indexes.at(i) + _fluxes_per_track + 1;
    int max_idx = _track_message_size * TRACKS_PER_BUFFER;
    for (int idx = start_idx; idx < max_idx; idx += _track_message_size) {
      long* track_info_location =
        reinterpret_cast<long*>(&_send_buffers.at(i)[idx]);
      track_info_location[0] = -1;
    }
  }

  //FIXME
  for (int i=0; i < num_domains; i++) {
    FP_PRECISION* buffer = _send_buffers.at(i);
    for (int t=0; t < TRACKS_PER_BUFFER; t++) {
      FP_PRECISION* curr_track_buffer = &buffer[t*_track_message_size];
      long* track_idx =
            reinterpret_cast<long*>(&curr_track_buffer[_fluxes_per_track+1]);
      long track_id = track_idx[0];
      if (track_id > _tot_num_tracks || track_id < -1) {
        std::cout << "Found tid = " << track_id << std::endl;
        std::cout << "TOT NUM TRACKS = " << _tot_num_tracks << std::endl;
        std::cout << "ADDRESS = " << &curr_track_buffer[_fluxes_per_track+1]
          << std::endl;
        exit(1);
      }
    }
  }
}


/**
 * @brief Transfers all angular fluxes at interfaces to their appropriate
 *        domain neighbors
 * @details The angular fluxes stored in the _boundary_flux array that
 *          intersect INTERFACE boundaries are transfered to their appropriate
 *          neighbor's _start_flux array at the periodic indexes.
 */
void CPUSolver::transferAllInterfaceFluxesNew() {

  /* Initialize buffer for MPI communication */
  MPI_Datatype flux_type;
  if (sizeof(FP_PRECISION) == 4)
    flux_type = MPI_FLOAT;
  else
    flux_type = MPI_DOUBLE;

  /* Create bookkeeping vectors */
  std::vector<long> packing_indexes;
  std::vector<int> buffer_indexes;
  std::vector<bool> active;

  /* Resize vectors to the number of domains */
  int num_domains = _neighbor_domains.size();
  packing_indexes.resize(num_domains);
  buffer_indexes.resize(num_domains);
  active.resize(num_domains);

  /* Initialize MPI requests and status */
  MPI_Comm MPI_cart = _geometry->getMPICart();
  MPI_Status stat;

  /* Start communication rounds */
  while (true) {

    int rank;
    MPI_Comm_rank(MPI_cart, &rank);

    if (rank == 1) {
      FP_PRECISION* buffer = _send_buffers.at(0);
      long * ti =
        reinterpret_cast<long*>(&buffer[17 + 1]);
      long t2 = ti[0];
      std::cout << "TSF x0 " << t2 << std::endl;
    }

    /* Pack buffers with angular flux data */
    for (int i=0; i < num_domains; i++)
      buffer_indexes.at(i) = 0;
    packBuffers(packing_indexes, buffer_indexes);

    if (rank == 1) {
      FP_PRECISION* buffer = _send_buffers.at(0);
      long * ti =
        reinterpret_cast<long*>(&buffer[17 + 1]);
      long t2 = ti[0];
      std::cout << "TSF " << t2 << std::endl;
      if (t2 != 4585) {
        std::cout << "WTF " << std::endl;
        exit(1);
      }
    }


    /* Send and receive from all neighboring domains */
    bool communication_complete = true;
    for (int i=0; i < num_domains; i++) {

      /* Get the communicating neighbor domain */
      int domain = _neighbor_domains.at(i);

      /* Check if a send/receive needs to be created */
      long* first_track_idx =
        reinterpret_cast<long*>(&_send_buffers.at(i)[_fluxes_per_track+1]);
      long first_track = first_track_idx[0];
      if (first_track != -1) {

        /* Send outgoing flux */
        MPI_Isend(&_send_buffers.at(i), _track_message_size *
                  TRACKS_PER_BUFFER, flux_type, domain, 0, MPI_cart,
                  &_MPI_requests[i*2]);
        _MPI_sends[i] = true;

        /* Receive incoming flux */
        MPI_Irecv(&_receive_buffers.at(i), _track_message_size *
                  TRACKS_PER_BUFFER, flux_type, domain, 0, MPI_cart,
                  &_MPI_requests[i*2+1]);
        _MPI_receives[i] = true;

        /* Mark communication as ongoing */
        communication_complete = false;
      }
    }

    /* Check if communication is done */
    if (communication_complete)
      break;

    if (rank == 1) {
      FP_PRECISION* buffer = _send_buffers.at(0);
      long * ti =
        reinterpret_cast<long*>(&buffer[17 + 1]);
      long t2 = ti[0];
      std::cout << "TSF xM1 " << t2 << std::endl;
    }

    /* Block for communication round to complete */
    bool round_complete = false;
    while (!round_complete) {

      round_complete = true;
      int flag;

      /* Check forward and backward send/receive messages */
      for (int i=0; i < num_domains; i++) {

        /* Wait for send to complete */
        if (_MPI_sends[i] == true) {
          MPI_Test(&_MPI_requests[i*2], &flag, &stat);
          if (flag == 0)
            round_complete = false;
        }

        /* Wait for receive to complete */
        if (_MPI_receives[i] == true) {
          MPI_Test(&_MPI_requests[i*2+1], &flag, &stat);
          if (flag == 0)
            round_complete = false;
        }
      }
    }

    if (rank == 1) {
      FP_PRECISION* buffer = _send_buffers.at(0);
      long * ti =
        reinterpret_cast<long*>(&buffer[17 + 1]);
      long t2 = ti[0];
      std::cout << "TSF xM2 " << t2 << std::endl;
    }

    /* Reset status for next communication round and copy fluxes */
    for (int i=0; i < num_domains; i++) {

      /* Reset send */
      _MPI_sends[i] = false;

      /* Copy angular fluxes if necessary */
      if (_MPI_receives[i]) {
        std::cout << "RECEIVING!!!" << std::endl;
        std::cout << "TBF = " << TRACKS_PER_BUFFER << std::endl;

        /* Ge the buffer for the connecting domain */
        FP_PRECISION* buffer = _receive_buffers.at(i);
        for (int t=0; t < TRACKS_PER_BUFFER; t++) {

          /* Get the Track ID */
          FP_PRECISION* curr_track_buffer = &buffer[t*_track_message_size];
          long* track_idx =
            reinterpret_cast<long*>(&curr_track_buffer[_fluxes_per_track+1]);
          long track_id = track_idx[0];

          std::cout << "MESSAGE " << t << std::endl;
          for (int g=0; g<_fluxes_per_track; g++)
            std::cout << "Flux " << g << " = " << curr_track_buffer[g] << std::endl;
          std::cout << "Direction " << curr_track_buffer[_fluxes_per_track]
            << std::endl;
          std::cout << "TID = " << track_id << std::endl;

          /* Check if the angular fluxes are active */
          if (track_id != -1) {
            int dir = curr_track_buffer[_fluxes_per_track];
            std::cout << "TRACK ID = " << track_id << std::endl;
            std::cout << "DIR = " << dir << std::endl;
            std::cout << "MAX TRACK = " << _tot_num_tracks << std::endl;
            std::cout << "YADA " << buffer[(t+1)*_track_message_size] << std::endl;
            for (int pe=0; pe < _fluxes_per_track; pe++)
              _start_flux(track_id, dir, pe) = curr_track_buffer[pe];
          }
        }
      }

      /* Reset receive */
      _MPI_receives[i] = false;
      MPI_Barrier(MPI_cart);
    }
    if (rank == 1) {
      FP_PRECISION* buffer = _send_buffers.at(0);
      long * ti =
        reinterpret_cast<long*>(&buffer[17 + 1]);
      long t2 = ti[0];
      std::cout << "TSF xF " << t2 << std::endl;
    }
  }

  /* Join MPI at the end of communication */
  MPI_Barrier(MPI_cart);
}


/**
 * @brief Transfers all angular fluxes at interfaces to their appropriate
 *        domain neighbors
 * @details The angular fluxes stored in the _boundary_flux array that
 *          intersect INTERFACE boundaries are transfered to their appropriate
 *          neighbor's _start_flux array at the periodic indexes.
 */
void CPUSolver::transferAllInterfaceFluxes() {

  /* Initialize buffer for MPI communication */
  //printCycle(1051,0,2000); //FIXME - this is broken for 2x2x2
  FP_PRECISION buffer[2*_fluxes_per_track];
  MPI_Datatype flux_type;
  if (sizeof(FP_PRECISION) == 4)
    flux_type = MPI_FLOAT;
  else
    flux_type = MPI_DOUBLE;

  /* Initialize MPI requests and status */
  MPI_Comm MPI_cart = _geometry->getMPICart();
  MPI_Status stat;
  MPI_Request request[4];
  int mpi_send[2] = {0};
  int mpi_recv[2] = {0};

  /* Loop over all tracks and exchange fluxes */
  for (int t=0; t < _tot_num_tracks; t++) {

    /* Get 3D Track data */
    TrackStackIndexes tsi;
    Track3D track;
    TrackGenerator3D* track_generator_3D =
      dynamic_cast<TrackGenerator3D*>(_track_generator);
    track_generator_3D->getTSIByIndex(t, &tsi);
    track_generator_3D->getTrackOTF(&track, &tsi);

    /* Get connecting tracks */
    int connect[2];
    connect[0] = track.getTrackPrdcFwd();
    connect[1] = track.getTrackPrdcBwd();

    /* Get connecting domain surfaces */
    int domain_fwd_in = track.getDomainFwdIn();
    int domain_fwd_out = track.getDomainFwdOut();
    int domain_bwd_in = track.getDomainBwdIn();
    int domain_bwd_out = track.getDomainBwdOut();

    int rank;
    MPI_Comm_rank(MPI_cart, &rank);

    /* Send outgoing flux */
    if (domain_fwd_out != -1) {
      MPI_Isend(&_boundary_flux(t,0,0), _fluxes_per_track, flux_type,
                domain_fwd_out, 0, MPI_cart, &request[0]);
      mpi_send[0] = 1;
    }

    /* Receive incoming flux on connecting Track */
    if (domain_fwd_in != -1) {
      MPI_Irecv(&buffer[0], _fluxes_per_track, flux_type, domain_fwd_in, 0,
                MPI_cart, &request[2]);
      mpi_recv[0] = 1;
    }

    /* Send backward flux */
    if (domain_bwd_out != -1) {
      MPI_Isend(&_boundary_flux(t,1,0), _fluxes_per_track, flux_type,
                domain_bwd_out, 0, MPI_cart, &request[1]);
      mpi_send[1] = 1;
    }

    /* Receive backward flux on connecting Track */
    if (domain_bwd_in != -1) {
      MPI_Irecv(&buffer[_fluxes_per_track], _fluxes_per_track, flux_type,
                domain_bwd_in, 0, MPI_cart, &request[3]);
      mpi_recv[1] = 1;
    }


    /* Block for communication round to complete */
    bool complete = false;
    while (!complete) {

      complete = true;
      int flag;

      /* Check forward and backward send/receive messages */
      for (int d=0; d<2; d++) {

        /* Wait for send to complete */
        if (mpi_send[d] == 1) {
          MPI_Test(&request[d], &flag, &stat);
          if (flag == 0)
            complete = false;
        }

        /* Wait for receive to complete */
        if (mpi_recv[d] == 1) {
          MPI_Test(&request[2+d], &flag, &stat);
          if (flag == 0)
            complete = false;
        }
      }
    }

    /* Reset status for next communication round and copy fluxes */
    for (int d=0; d<2; d++) {

      /* Reset send */
      mpi_send[d] = 0;

      /* Copy angular fluxes if necessary */
      if (mpi_recv[d])
        for (int pe=0; pe < _fluxes_per_track; pe++)
          _start_flux(connect[d],d,pe) =
            buffer[d*_fluxes_per_track + pe];

      /* Reset receive */
      mpi_recv[d] = 0;
    }
  }

  /* Join MPI at the end of communication */
  MPI_Barrier(MPI_cart);
}
#endif


/**
 * @brief Set the scalar flux for each FSR and energy group to some value.
 * @param value the value to assign to each FSR scalar flux
 */
void CPUSolver::flattenFSRFluxes(FP_PRECISION value) {

#pragma omp parallel for schedule(guided)
  for (int r=0; r < _num_FSRs; r++) {
    for (int e=0; e < _num_groups; e++)
      _scalar_flux(r,e) = value;
  }
}


/**
 * @brief Stores the FSR scalar fluxes in the old scalar flux array.
 */
void CPUSolver::storeFSRFluxes() {

#pragma omp parallel for schedule(guided)
  for (int r=0; r < _num_FSRs; r++) {
    for (int e=0; e < _num_groups; e++)
      _old_scalar_flux(r,e) = _scalar_flux(r,e);
  }
}


/**
 * @brief Normalizes all FSR scalar fluxes and Track boundary angular
 *        fluxes to the total fission source (times \f$ \nu \f$).
 */
void CPUSolver::normalizeFluxes() {

  FP_PRECISION* nu_sigma_f;
  FP_PRECISION volume;
  FP_PRECISION tot_fission_source;
  FP_PRECISION norm_factor;

  int size = _num_FSRs * _num_groups;
  FP_PRECISION* fission_sources = new FP_PRECISION[_num_FSRs * _num_groups];

  /* Compute total fission source for each FSR, energy group */
#pragma omp parallel for private(volume, nu_sigma_f) schedule(guided)
  for (int r=0; r < _num_FSRs; r++) {

    /* Get pointers to important data structures */
    nu_sigma_f = _FSR_materials[r]->getNuSigmaF();
    volume = _FSR_volumes[r];

    for (int e=0; e < _num_groups; e++)
      fission_sources(r,e) = nu_sigma_f[e] * _scalar_flux(r,e) * volume;
  }

  /* Compute the total fission source */
  tot_fission_source = pairwise_sum<FP_PRECISION>(fission_sources,size);

#ifdef MPIx
  /* Reduce total fission rates across domians */
  if (_geometry->isDomainDecomposed()) {

    /* Get the communicator */
    MPI_Comm comm = _geometry->getMPICart();

    /* Determine the floating point precision */
    FP_PRECISION reduced_fission;
    MPI_Datatype precision;
    if (sizeof(FP_PRECISION) == 4)
      precision = MPI_FLOAT;
    else
      precision = MPI_DOUBLE;

    /* Reduce fission rates */
    MPI_Allreduce(&tot_fission_source, &reduced_fission, 1, precision,
                  MPI_SUM, comm);
    tot_fission_source = reduced_fission;
  }
#endif

  /* Deallocate memory for fission source array */
  delete [] fission_sources;

  /* Normalize scalar fluxes in each FSR */
  norm_factor = 1.0 / tot_fission_source;

  log_printf(DEBUG, "Tot. Fiss. Src. = %f, Norm. factor = %f",
             tot_fission_source, norm_factor);

#pragma omp parallel for schedule(guided)
  for (int r=0; r < _num_FSRs; r++) {
    for (int e=0; e < _num_groups; e++) {
      _scalar_flux(r, e) *= norm_factor;
      _old_scalar_flux(r, e) *= norm_factor;
    }
  }

  /* Normalize angular boundary fluxes for each Track */
#pragma omp parallel for schedule(guided)
  for (int i=0; i < _tot_num_tracks; i++) {
    for (int j=0; j < 2; j++) {
      for (int pe=0; pe < _fluxes_per_track; pe++) {
        _start_flux(i, j, pe) *= norm_factor;
        _boundary_flux(i, j, pe) *= norm_factor;
      }
    }
  }
}


/**
 * @brief Computes the total source (fission, scattering, fixed) in each FSR.
 * @details This method computes the total source in each FSR based on
 *          this iteration's current approximation to the scalar flux.
 */
void CPUSolver::computeFSRSources() {

  int tid;
  FP_PRECISION scatter_source, fission_source;
  FP_PRECISION* nu_sigma_f;
  FP_PRECISION* sigma_t;
  FP_PRECISION* chi;
  Material* material;

  int size = _num_FSRs * _num_groups;
  FP_PRECISION* fission_sources = new FP_PRECISION[size];
  size = _num_threads * _num_groups;
  FP_PRECISION* scatter_sources = new FP_PRECISION[size];

  /* For all FSRs, find the source */
#pragma omp parallel for private(tid, material, nu_sigma_f, chi, \
    sigma_t, fission_source, scatter_source) schedule(guided)
  for (int r=0; r < _num_FSRs; r++) {

    tid = omp_get_thread_num();
    material = _FSR_materials[r];
    nu_sigma_f = material->getNuSigmaF();
    chi = material->getChi();
    sigma_t = material->getSigmaT();

    /* Initialize the fission sources to zero */
    fission_source = 0.0;

    /* Compute fission source for each group */
    if (material->isFissionable()) {
      for (int e=0; e < _num_groups; e++)
        fission_sources(r,e) = _scalar_flux(r,e) * nu_sigma_f[e];

      fission_source = pairwise_sum<FP_PRECISION>(&fission_sources(r,0),
                                                  _num_groups);
      fission_source /= _k_eff;
    }

    /* Compute total (fission+scatter+fixed) source for group G */
    for (int G=0; G < _num_groups; G++) {
      for (int g=0; g < _num_groups; g++)
        scatter_sources(tid,g) = material->getSigmaSByGroup(g+1,G+1)
                                  * _scalar_flux(r,g);
      scatter_source = pairwise_sum<FP_PRECISION>(&scatter_sources(tid,0),
                                                _num_groups);

      _reduced_sources(r,G) = fission_source * chi[G];
      _reduced_sources(r,G) += scatter_source + _fixed_sources(r,G);
      _reduced_sources(r,G) *= ONE_OVER_FOUR_PI / sigma_t[G];
    }
  }

  delete [] fission_sources;
  delete [] scatter_sources;
}


/**
 * @brief Computes the residual between source/flux iterations.
 * @param res_type the type of residuals to compute
 *        (SCALAR_FLUX, FISSION_SOURCE, TOTAL_SOURCE)
 * @return the average residual in each FSR
 */
double CPUSolver::computeResidual(residualType res_type) {

  int norm;
  double residual;
  double* residuals = new double[_num_FSRs];
  memset(residuals, 0., _num_FSRs * sizeof(double));

  if (res_type == SCALAR_FLUX) {

    norm = _num_FSRs;

    for (int r=0; r < _num_FSRs; r++) {
      for (int e=0; e < _num_groups; e++)
        if (_old_scalar_flux(r,e) > 0.) {
          residuals[r] += pow((_scalar_flux(r,e) - _old_scalar_flux(r,e)) /
                              _old_scalar_flux(r,e), 2);
      }
    }
  }

  else if (res_type == FISSION_SOURCE) {

    if (_num_fissionable_FSRs == 0)
      log_printf(ERROR, "The Solver is unable to compute a "
                 "FISSION_SOURCE residual without fissionable FSRs");

    norm = _num_fissionable_FSRs;

    double new_fission_source, old_fission_source;
    FP_PRECISION* nu_sigma_f;
    Material* material;

    for (int r=0; r < _num_FSRs; r++) {
      new_fission_source = 0.;
      old_fission_source = 0.;
      material = _FSR_materials[r];

      if (material->isFissionable()) {
        nu_sigma_f = material->getNuSigmaF();

        for (int e=0; e < _num_groups; e++) {
          new_fission_source += _scalar_flux(r,e) * nu_sigma_f[e];
          old_fission_source += _old_scalar_flux(r,e) * nu_sigma_f[e];
        }

        if (old_fission_source > 0.)
          residuals[r] = pow((new_fission_source -  old_fission_source) /
                              old_fission_source, 2);
      }
    }
  }

  else if (res_type == TOTAL_SOURCE) {

    norm = _num_FSRs;

    double new_total_source, old_total_source;
    FP_PRECISION inverse_k_eff = 1.0 / _k_eff;
    FP_PRECISION* nu_sigma_f;
    Material* material;

    for (int r=0; r < _num_FSRs; r++) {
      new_total_source = 0.;
      old_total_source = 0.;
      material = _FSR_materials[r];

      if (material->isFissionable()) {
        nu_sigma_f = material->getNuSigmaF();

        for (int e=0; e < _num_groups; e++) {
          new_total_source += _scalar_flux(r,e) * nu_sigma_f[e];
          old_total_source += _old_scalar_flux(r,e) * nu_sigma_f[e];
        }

        new_total_source *= inverse_k_eff;
        old_total_source *= inverse_k_eff;
      }

      /* Compute total scattering source for group G */
      for (int G=0; G < _num_groups; G++) {
        for (int g=0; g < _num_groups; g++) {
          new_total_source += material->getSigmaSByGroup(g+1,G+1)
                              * _scalar_flux(r,g);
          old_total_source += material->getSigmaSByGroup(g+1,G+1)
                              * _old_scalar_flux(r,g);
        }
      }

      if (old_total_source > 0.)
        residuals[r] = pow((new_total_source -  old_total_source) /
                            old_total_source, 2);
    }
  }

  /* Sum up the residuals from each FSR and normalize */
  residual = pairwise_sum<double>(residuals, _num_FSRs);

#ifdef MPIx
  /* Reduce residuals across domians */
  if (_geometry->isDomainDecomposed()) {

    /* Get the communicator */
    MPI_Comm comm = _geometry->getMPICart();

    /* Reduce residuals */
    double reduced_res;
    MPI_Allreduce(&residual, &reduced_res, 1, MPI_DOUBLE, MPI_SUM, comm);
    residual = reduced_res;

    /* Reduce normalization factors */
    int reduced_norm;
    MPI_Allreduce(&norm, &reduced_norm, 1, MPI_INT, MPI_SUM, comm);
    norm = reduced_norm;
  }
#endif

  /* Compute RMS residual */
  residual = sqrt(residual / norm);

  /* Deallocate memory for residuals array */
  delete [] residuals;

  return residual;
}


/**
 * @brief Compute \f$ k_{eff} \f$ from successive fission sources.
 */
void CPUSolver::computeKeff() {

  int tid;
  Material* material;
  FP_PRECISION* sigma;
  FP_PRECISION volume;

  FP_PRECISION fission;
  FP_PRECISION* FSR_rates = new FP_PRECISION[_num_FSRs];
  FP_PRECISION* group_rates = new FP_PRECISION[_num_threads * _num_groups];

  /* Loop over all FSRs and compute the volume-integrated total rates */
#pragma omp parallel for private(tid, volume, \
    material, sigma) schedule(guided)
  for (int r=0; r < _num_FSRs; r++) {

    tid = omp_get_thread_num() * _num_groups;
    volume = _FSR_volumes[r];
    material = _FSR_materials[r];
    sigma = material->getNuSigmaF();

    for (int e=0; e < _num_groups; e++)
      group_rates[tid+e] = sigma[e] * _scalar_flux(r,e);

    FSR_rates[r]=pairwise_sum<FP_PRECISION>(&group_rates[tid], _num_groups);
    FSR_rates[r] *= volume;
  }

  /* Reduce new fission rates across FSRs */
  fission = pairwise_sum<FP_PRECISION>(FSR_rates, _num_FSRs);

#ifdef MPIx
  /* Reduce fission rates across domians */
  if (_geometry->isDomainDecomposed()) {

    /* Get the communicator */
    MPI_Comm comm = _geometry->getMPICart();

    /* Determine the floating point precision */
    FP_PRECISION reduced_fission;
    MPI_Datatype precision;
    if (sizeof(FP_PRECISION) == 4)
      precision = MPI_FLOAT;
    else
      precision = MPI_DOUBLE;

    /* Reduce fission rates */
    MPI_Allreduce(&fission, &reduced_fission, 1, precision, MPI_SUM, comm);
    fission = reduced_fission;
  }
#endif

  /* The old_source is normalized to sum to _k_eff; therefore, the new
   * _k_eff is the old _k_eff * sum(new_source). Implicitly, we are just doing
   * _k_eff = sum(new_source) / sum(old_source). */
  _k_eff *= fission;

  delete [] FSR_rates;
  delete [] group_rates;
}


/**
 * @brief This method performs one transport sweep of all azimuthal angles,
 *        Tracks, Track segments, polar angles and energy groups.
 * @details The method integrates the flux along each Track and updates the
 *          boundary fluxes for the corresponding output Track, while updating
 *          the scalar flux in each flat source region.
 */
void CPUSolver::transportSweep() {

  log_printf(DEBUG, "Transport sweep with %d OpenMP threads",
      _num_threads);

  if (_cmfd != NULL && _cmfd->isFluxUpdateOn())
    _cmfd->zeroCurrents();

  /* Initialize flux in each FSR to zero */
  flattenFSRFluxes(0.0);

  /* Copy starting flux to current flux */
  copyBoundaryFluxes();

  /* Tracks are traversed and the MOC equations from this CPUSolver are applied
     to all Tracks and corresponding segments */
  if (_OTF_transport) {
    TransportSweepOTF sweep_tracks(_track_generator);
    sweep_tracks.setCPUSolver(this);
    sweep_tracks.execute();
  }
  else {
    TransportSweep sweep_tracks(_track_generator);
    sweep_tracks.setCPUSolver(this);
    sweep_tracks.execute();
  }

#ifdef MPIx
  /* Transfer all interface fluxes after the transport sweep */
  if (_track_generator->getGeometry()->isDomainDecomposed())
    transferAllInterfaceFluxesNew();
#endif
}


/**
 * @brief Computes the contribution to the FSR scalar flux from a Track segment.
 * @details This method integrates the angular flux for a Track segment across
 *          energy groups and polar angles, and tallies it into the FSR
 *          scalar flux, and updates the Track's angular flux.
 * @param curr_segment a pointer to the Track segment of interest
 * @param azim_index a pointer to the azimuthal angle index for this segment
 * @param track_flux a pointer to the Track's angular flux
 * @param fsr_flux a pointer to the temporary FSR flux buffer
 */
void CPUSolver::tallyScalarFlux(segment* curr_segment,
                                int azim_index, int polar_index,
                                FP_PRECISION* track_flux,
                                FP_PRECISION* fsr_flux) {

  int fsr_id = curr_segment->_region_id;
  FP_PRECISION length = curr_segment->_length;
  FP_PRECISION* sigma_t = curr_segment->_material->getSigmaT();

  /* The change in angular flux along this Track segment in the FSR */
  FP_PRECISION delta_psi, exponential, tau;

  /* Set the FSR scalar flux buffer to zero */
  memset(fsr_flux, 0.0, _num_groups * sizeof(FP_PRECISION));

  if (_solve_3D) {

    for (int e=0; e < _num_groups; e++) {
      exponential = _exp_evaluator->computeExponential
        (sigma_t[e] * length, azim_index, polar_index);
      delta_psi = (track_flux[e]-_reduced_sources(fsr_id, e)) * exponential;
      fsr_flux[e] += delta_psi * _quad->getWeightInline(azim_index,
                                                        polar_index);
      track_flux[e] -= delta_psi;
    }
  }
  else {

    int pe = 0;

    /* Loop over energy groups */
    for (int e=0; e < _num_groups; e++) {

      tau = sigma_t[e] * length;

      /* Loop over polar angles */
      for (int p=0; p < _num_polar/2; p++) {
        exponential = _exp_evaluator->computeExponential(tau, azim_index, p);
        delta_psi = (track_flux[pe]-_reduced_sources(fsr_id,e)) * exponential;
        fsr_flux[e] += delta_psi * _quad->getWeightInline(azim_index, p);
        track_flux[pe] -= delta_psi;
        pe++;
      }
    }
  }

  /* Atomically increment the FSR scalar flux from the temporary array */
  omp_set_lock(&_FSR_locks[fsr_id]);
  {
    for (int e=0; e < _num_groups; e++)
      _scalar_flux(fsr_id,e) += fsr_flux[e];
  }
  omp_unset_lock(&_FSR_locks[fsr_id]);
}


/**
 * @brief Tallies the current contribution from this segment across the
 *        the appropriate CMFD mesh cell surface.
 * @param curr_segment a pointer to the Track segment of interest
 * @param azim_index the azimuthal index for this segmenbt
 * @param track_flux a pointer to the Track's angular flux
 * @param fwd boolean indicating direction of integration along segment
 */
void CPUSolver::tallyCurrent(segment* curr_segment, int azim_index,
                             int polar_index, FP_PRECISION* track_flux,
                             bool fwd) {

  /* Tally surface currents if CMFD is in use */
  if (_cmfd != NULL && _cmfd->isFluxUpdateOn())
    _cmfd->tallyCurrent(curr_segment, track_flux, azim_index, polar_index, fwd);
}


/**
 * @brief Updates the boundary flux for a Track given boundary conditions.
 * @details For reflective boundary conditions, the outgoing boundary flux
 *          for the Track is given to the reflecting Track. For vacuum
 *          boundary conditions, the outgoing flux tallied as leakage.
 * @param track_id the ID number for the Track of interest
 * @param azim_index a pointer to the azimuthal angle index for this segment
 * @param direction the Track direction (forward - true, reverse - false)
 * @param track_flux a pointer to the Track's outgoing angular flux
 */
void CPUSolver::transferBoundaryFlux(Track* track,
                                     int azim_index, int polar_index,
                                     bool direction,
                                     FP_PRECISION* track_flux) {

  /* Extract boundary conditions for this Track and the pointer to the
   * outgoing reflective Track, and index into the leakage array */
  boundaryType bc;
  long track_out_id;
  int start;

  /* For the "forward" direction */
  if (direction) {
    bc = track->getBCFwd();
    track_out_id = track->getTrackNextFwd();
    start = _fluxes_per_track * (!track->getNextFwdFwd());
  }

  /* For the "reverse" direction */
  else {
    bc = track->getBCBwd();
    track_out_id = track->getTrackNextBwd();
    start = _fluxes_per_track * (!track->getNextBwdFwd());
  }

  FP_PRECISION* track_out_flux = &_start_flux(track_out_id, 0, start);

  /* Determine if flux should be transferred */
  int transfer = (bc != VACUUM);

  if (bc != INTERFACE)
    for (int pe=0; pe < _fluxes_per_track; pe++)
      track_out_flux[pe] = track_flux[pe] * transfer;
}


/**
 * @brief Add the source term contribution in the transport equation to
 *        the FSR scalar flux.
 */
void CPUSolver::addSourceToScalarFlux() {

  FP_PRECISION volume;
  FP_PRECISION* sigma_t;

  /* Add in source term and normalize flux to volume for each FSR */
  /* Loop over FSRs, energy groups */
#pragma omp parallel for private(volume, sigma_t) schedule(guided)
  for (int r=0; r < _num_FSRs; r++) {
    volume = _FSR_volumes[r];
    sigma_t = _FSR_materials[r]->getSigmaT();

    for (int e=0; e < _num_groups; e++) {
      _scalar_flux(r, e) /= (sigma_t[e] * volume);
      _scalar_flux(r, e) += FOUR_PI * _reduced_sources(r, e);
    }
  }

  return;
}


/**
 * @brief Computes the volume-averaged, energy-integrated nu-fission rate in
 *        each FSR and stores them in an array indexed by FSR ID.
 * @details This is a helper method for SWIG to allow users to retrieve
 *          FSR nu-fission rates as a NumPy array. An example of how this
 *          method can be called from Python is as follows:
 *
 * @code
 *          num_FSRs = geometry.getNumFSRs()
 *          fission_rates = solver.computeFSRFissionRates(num_FSRs)
 * @endcode
 *
 * @param fission_rates an array to store the nu-fission rates (implicitly
 *                      passed in as a NumPy array from Python)
 * @param num_FSRs the number of FSRs passed in from Python
 */
void CPUSolver::computeFSRFissionRates(double* fission_rates, int num_FSRs) {

  if (_scalar_flux == NULL)
    log_printf(ERROR, "Unable to compute FSR fission rates since the "
               "source distribution has not been calculated");

  log_printf(INFO, "Computing FSR fission rates...");

  FP_PRECISION* nu_sigma_f;

  /* Initialize fission rates to zero */
  for (int r=0; r < _num_FSRs; r++)
    fission_rates[r] = 0.0;

  /* Loop over all FSRs and compute the volume-weighted fission rate */
#pragma omp parallel for private (nu_sigma_f) schedule(guided)
  for (int r=0; r < _num_FSRs; r++) {
    nu_sigma_f = _FSR_materials[r]->getNuSigmaF();

    for (int e=0; e < _num_groups; e++)
      fission_rates[r] += nu_sigma_f[e] * _scalar_flux(r,e);
  }
}
