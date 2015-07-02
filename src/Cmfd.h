/**
 * @file Cmfd.h
 * @brief The Cmfd class.
 * @date October 14, 2013
 * @author Sam Shaner, MIT, Course 22 (shaner@mit.edu)
 */

#ifndef CMFD_H_
#define CMFD_H_

#ifdef __cplusplus
#define _USE_MATH_DEFINES
#include "Python.h"
#include "log.h"
#include "Timer.h"
#include "Universe.h"
#include "Track2D.h"
#include "Track3D.h"
#include "Quadrature.h"
#include "linalg.h"
#include "Vector.h"
#include "Matrix.h"
#include "Geometry.h"
#include <utility>
#include <math.h>
#include <limits.h>
#include <string>
#include <sstream>
#include <queue>
#include <iostream>
#include <fstream>
#endif

/** Forward declaration of Geometry class */
class Geometry;

/** Comparitor for sorting k-nearest stencil std::pair objects */
inline bool stencilCompare(const std::pair<int, FP_PRECISION>& firstElem,
                           const std::pair<int, FP_PRECISION>& secondElem){
  return firstElem.second < secondElem.second;
}


/**
 * @class Cmfd Cmfd.h "src/Cmfd.h"
 * @brief A class for Coarse Mesh Finite Difference (CMFD) acceleration.
 */
class Cmfd {

private:
  
  /** Pointer to polar quadrature object */
  Quadrature* _quadrature;

  /** Pointer to geometry object */
  Geometry* _geometry;
  
  /** The keff eigenvalue */
  FP_PRECISION _k_eff;

  /** The A (destruction) matrix */
  Matrix* _A;

  /** The M (production) matrix */
  Matrix* _M;

  /** The old source vector */
  Vector* _old_source;

  /** The new source vector */
  Vector* _new_source;

  /** Vector representing the flux for each cmfd cell and cmfd enegy group at 
   * the end of a CMFD solve */
  Vector* _new_flux;

  /** Vector representing the flux for each cmfd cell and cmfd enegy group at 
   * the beginning of a CMFD solve */
  Vector* _old_flux;

  /** Gauss-Seidel SOR relaxation factor */
  FP_PRECISION _SOR_factor;

  /** cmfd source convergence threshold */
  FP_PRECISION _source_convergence_threshold;

  /** Number of cells in x direction */
  int _num_x;

  /** Number of cells in y direction */
  int _num_y;

  /** Number of cells in z direction */
  int _num_z;

  /** Number of energy groups */
  int _num_moc_groups;

  /** Number of polar angles */
  int _num_polar;

  /** Number of energy groups used in cmfd solver. Note that cmfd supports
   * energy condensation from the MOC */
  int _num_cmfd_groups;

  /** Coarse energy indices for fine energy groups */
  int* _group_indices;

  /** Map of MOC groups to CMFD groups */
  int* _group_indices_map;

  /** Number of FSRs */
  int _num_FSRs;

  /** The volumes (areas) for each FSR */
  FP_PRECISION* _FSR_volumes;

  /** Pointers to Materials for each FSR */
  Material** _FSR_materials;

  /** The FSR scalar flux in each energy group */
  FP_PRECISION* _FSR_fluxes;

  /** Array of CMFD cell volumes */
  Vector* _volumes;

  /** Array of material pointers for CMFD cell materials */
  Material** _materials;

  /** Physical dimensions of the geometry and each CMFD cell */
  FP_PRECISION _width;
  FP_PRECISION _height;
  FP_PRECISION _depth;
  FP_PRECISION _cell_width;
  FP_PRECISION _cell_height;
  FP_PRECISION _cell_depth;

  /** Array of geometry boundaries */
  boundaryType* _boundaries;

  /** Array of surface currents for each CMFD cell */
  Vector* _surface_currents;

  /** OpenMP mutual exclusion locks for atomic surface current updates */
  omp_lock_t* _surface_locks;

  /** Vector of vectors of FSRs containing in each cell */
  std::vector< std::vector<int> > _cell_fsrs;

  /** MOC flux update relaxation factor */
  FP_PRECISION _relax_factor;

  /** Flag indicating whether to use optically thick correction factor */
  bool _optically_thick;

  /** Pointer to Lattice object representing the CMFD mesh */
  Lattice* _lattice;

  /** Flag indicating whether to update the MOC flux */
  bool _flux_update_on;

  FP_PRECISION* _azim_spacings;
  FP_PRECISION** _polar_spacings;
  bool _solve_3D;

  /** Flag indicating whether to us centroid updating */
  bool _centroid_update_on;
  
  /** Number of cells to used in updating MOC flux */
  int _k_nearest;
  
  /** Map storing the k-nearest stencil for each fsr */
  std::map<int, std::vector< std::pair<int, FP_PRECISION> > >
    _k_nearest_stencils;
  
public:

  Cmfd();
  virtual ~Cmfd();

  /* Worker functions */
  void constructMatrices();
  void computeDs(int moc_iteration);
  void computeXS();
  void updateMOCFlux();
  FP_PRECISION computeDiffCorrect(FP_PRECISION d, FP_PRECISION h);
  FP_PRECISION computeKeff(int moc_iteration);
  void initializeCellMap();
  void initializeGroupMap();
  void initializeMaterials();
  void initializeSurfaceCurrents();

  void rescaleFlux();
  void splitCorners();
  int getCellNext(int cell_num, int surface_id);
  int findCmfdCell(LocalCoords* coords);
  int findCmfdSurface(int cell, LocalCoords* coords);
  void addFSRToCell(int cmfd_cell, int fsr_id);
  void generateKNearestStencils();
  void zeroSurfaceCurrents();
  void tallySurfaceCurrent(segment* curr_segment, FP_PRECISION* track_flux, 
                           int azim_index, int polar_index, bool fwd);

  /* Get parameters */
  int getNumCmfdGroups();
  int getNumMOCGroups();
  int getNumCells();
  int getCmfdGroup(int group);
  bool isOpticallyThick();
  FP_PRECISION getMOCRelaxationFactor();
  int getBoundary(int side);
  Lattice* getLattice();
  int getNumX();
  int getNumY();
  int getNumZ();
  int convertFSRIdToCmfdCell(int fsr_id);
  std::vector< std::vector<int> > getCellFSRs();
  bool isFluxUpdateOn();
  bool isCentroidUpdateOn();
  FP_PRECISION getFluxRatio(int cmfd_cell, int moc_group);
  FP_PRECISION getUpdateRatio(int cmfd_cell, int moc_group, int fsr);
  FP_PRECISION getDistanceToCentroid(Point* centroid, int cell, int surface);
  
  /* Set parameters */
  void setSORRelaxationFactor(FP_PRECISION SOR_factor);
  void setGeometry(Geometry* geometry);
  void setWidth(double width);
  void setHeight(double height);
  void setDepth(double depth);
  void setNumX(int num_x);
  void setNumY(int num_y);
  void setNumZ(int num_z);
  void setSurfaceCurrents(Vector* surface_currents);
  void setNumFSRs(int num_fsrs);
  void setNumMOCGroups(int num_moc_groups);
  void setOpticallyThick(bool thick);
  void setMOCRelaxationFactor(FP_PRECISION relax_factor);
  void setBoundary(int side, boundaryType boundary);
  void setLattice(Lattice* lattice);
  void setLatticeStructure(int num_x, int num_y, int num_z=1);
  void setFluxUpdateOn(bool flux_update_on);
  void setCentroidUpdateOn(bool centroid_update_on);
  void setGroupStructure(int* group_indices, int length_group_indices);
  void setSourceConvergenceThreshold(FP_PRECISION source_thresh);
  void setQuadrature(Quadrature* quadrature);
  void setKNearest(int k_nearest);
  void setAzimSpacings(double* azim_spacings, int num_azim);
  void setPolarSpacings(double** azim_spacings, int num_azim, int num_polar);
  void setSolve3D(bool solve_3d);
  
  /* Set FSR parameters */
  void setFSRMaterials(Material** FSR_materials);
  void setFSRVolumes(FP_PRECISION* FSR_volumes);
  void setFSRFluxes(FP_PRECISION* scalar_flux);
  void setCellFSRs(std::vector< std::vector<int> > cell_fsrs);
};

#endif /* CMFD_H_ */
