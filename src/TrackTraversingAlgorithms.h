/**
 * @file TrackTraversingAlgorithms.h
 * @brief Contains classes which extend the TraverseTracks class to apply
 *        algorithms to Tracks and possibly their segments
 * @details The classes defined within this file extend the TraverseTracks
 *          class so that they are capable of using the abstract looping
 *          defined in TraverseTracks::loopOverTracks(...). Each class
 *          contains a constructor which pulls data from a provided
 *          TrackGenerator, an onTrack(...) function which specifies what to do
 *          on each Track, and an execute() function which applies the
 *          algorithm. The execute() function should contain a call to
 *          TraverseTracks::loopOverTracks(...). To specify a behavior to
 *          be applied once for each segment, a kernel should be passed to
 *          TraverseTracks::loopOverTracks().
 * @date February 23, 2016
 * @author Geoffrey Gunow, MIT, Course 22 (geogunow@mit.edu)
 */

#ifndef TRACK_TRAVERSING_ALGORITHMS_H_
#define TRACK_TRAVERSING_ALGORITHMS_H_

#include "TraverseTracks.h"


/** Forward declaration of CPUSolver class */
class CPUSolver;


/**
 * @class MaxOpticalLength TrackTraversingAlgorithms.h
 *        "src/TrackTraversingAlgorithms.h"
 * @brief A class used to calculate the maximum optical path length across
 *        all segments in the Geometry
 * @details The segments are traversed and the maximium optical path length
 *          is calculated.
 */
class MaxOpticalLength: public TraverseTracks {
private:
  FP_PRECISION _max_tau;

public:

  MaxOpticalLength(TrackGenerator* track_generator);
  void execute();
  void onTrack(Track* track, segment* segments);
};


/**
 * @class SegmentSplitter TrackTraversingAlgorithms.h
 *        "src/TrackTraversingAlgorithms.h"
 * @brief A class used to split explicit segments along Tracks
 * @details A SegmentSplitter imports a maximum optical path length from the
 *          provided TrackGenerator and then ensures all segments have an
 *          optical path length less than the maximum optical path length by
 *          splitting segments stored in the Tracks.
 */
class SegmentSplitter: public TraverseTracks {

public:

  SegmentSplitter(TrackGenerator* track_generator);
  void execute();
  void onTrack(Track* track, segment* segments);
};

/**
 * @class VolumeCalculator TrackTraversingAlgorithms.h
 *        "src/TrackTraversingAlgorithms.h"
 * @brief A class used to calculate FSR volumes
 * @details A VolumeCalculator imports a buffer to store FSR volumes from the
 *          provided TrackGenerator and the allocates VolumeKernels to
 *          calculate and update the volumes in each FSR, implicitly writing
 *          the calculated volumes back to the TrackGenerator.
 */
class VolumeCalculator: public TraverseTracks {

public:

  VolumeCalculator(TrackGenerator* track_generator);
  void execute();
};


/**
 * @class CentroidGenerator TrackTraversingAlgorithms.h
 *        "src/TrackTraversingAlgorithms.h"
 * @brief A class used to calculate the centroids of each FSR
 * @details A CentroidGenerator imports FSR Volumes and associated locks form
 *          the provided TrackGenerator, then centroids are calculated and
 *          stored in the provided buffer by looping over all segments and
 *          adding their contribution to each FSR centroid.
 */
class CentroidGenerator: public TraverseTracks {

private:

  Point** _centroids;
  FP_PRECISION* _FSR_volumes;
  omp_lock_t* _FSR_locks;
  Quadrature* _quadrature;
  Point** _starting_points;
  bool** _new_track;

public:

  CentroidGenerator(TrackGenerator* track_generator);
  void setCentroids(Point** centroids);
  void execute();
  void onTrack(Track* track, segment* segments);
};


/**
 * @class TransportSweep TrackTraversingAlgorithms.h
 *        "src/TrackTraversingAlgorithms.h"
 * @brief A class used to apply the MOC transport equations to all segments
 * @details TransportSweep imports data from the provided TrackGenerator and
 *          using a provided CPUSolver, it applies the MOC equations to each
 *          segment, tallying the contributions to each FSR. At the end of each
 *          Track, boundary fluxes are exchanged based on boundary conditions.
 */
class TransportSweep: public TraverseTracks {

private:

  CPUSolver* _cpu_solver;

public:

  TransportSweep(TrackGenerator* track_generator);
  void setCPUSolver(CPUSolver* cpu_solver);
  void execute();
  void onTrack(Track* track, segment* segments);
};


/**
 * @class DumpSegments TrackTraversingAlgorithms.h
 *        "src/TrackTraversingAlgorithms.h"
 * @brief A class used to write tracking data to a file
 * @details DumpSegments imports Track data from the provided TrackGenerator
 *          and writes the tracking data to the provided file.
 */
class DumpSegments: public TraverseTracks {

private:

  FILE* _out;

public:

  DumpSegments(TrackGenerator* track_generator);
  void setOutputFile(FILE* out);
  void execute();
  void onTrack(Track* track, segment* segments);
};


/**
 * @class ReadSegments TrackTraversingAlgorithms.h
 *        "src/TrackTraversingAlgorithms.h"
 * @brief A class used to read tracking data from a file.
 * @details ReadSegments imports Track data from the provided file and writes
 *          the tracking data to the Tracks in the provided TrackGenerator.
 */
class ReadSegments: public TraverseTracks {

private:

  FILE* _in;
  Quadrature* _quadrature;
  int _num_azim_2;

public:

  ReadSegments(TrackGenerator* track_generator);
  void setInputFile(FILE* in);
  void execute();
  void onTrack(Track* track, segment* segments);
};


#endif
