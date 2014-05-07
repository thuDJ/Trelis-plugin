/*
 * NCWriteGCRM.cpp
 *
 *  Created on: April 9, 2014
 */

#include "NCWriteGCRM.hpp"
#include "moab/WriteUtilIface.hpp"
#include "MBTagConventions.hpp"

#define ERRORR(rval, str) \
  if (MB_SUCCESS != rval) { _writeNC->mWriteIface->report_error("%s", str); return rval; }

#define ERRORS(err, str) \
  if (err) { _writeNC->mWriteIface->report_error("%s", str); return MB_FAILURE; }

namespace moab {

NCWriteGCRM::~NCWriteGCRM()
{
  // TODO Auto-generated destructor stub
}

ErrorCode NCWriteGCRM::collect_mesh_info()
{
  Interface*& mbImpl = _writeNC->mbImpl;
  std::vector<std::string>& dimNames = _writeNC->dimNames;
  std::vector<int>& dimLens = _writeNC->dimLens;
  Tag& mGlobalIdTag = _writeNC->mGlobalIdTag;

  ErrorCode rval;

  // Look for time dimension
  std::vector<std::string>::iterator vecIt;
  if ((vecIt = std::find(dimNames.begin(), dimNames.end(), "Time")) != dimNames.end())
    tDim = vecIt - dimNames.begin();
  else if ((vecIt = std::find(dimNames.begin(), dimNames.end(), "time")) != dimNames.end())
    tDim = vecIt - dimNames.begin();
  else {
    ERRORR(MB_FAILURE, "Couldn't find 'Time' or 'time' dimension.");
  }
  nTimeSteps = dimLens[tDim];

  // Get number of levels
  if ((vecIt = std::find(dimNames.begin(), dimNames.end(), "nVertLevels")) != dimNames.end())
    levDim = vecIt - dimNames.begin();
  else {
    ERRORR(MB_FAILURE, "Couldn't find 'nVertLevels' dimension.");
  }
  nLevels = dimLens[levDim];

  // Get local vertices
  rval = mbImpl->get_entities_by_dimension(_fileSet, 0, localVertsOwned);
  ERRORR(rval, "Trouble getting local vertices in current file set.");
  assert(!localVertsOwned.empty());

  // Get local edges
  rval = mbImpl->get_entities_by_dimension(_fileSet, 1, localEdgesOwned);
  ERRORR(rval, "Trouble getting local edges in current file set.");
  // There are no edges if NO_EDGES read option is set

  // Get local cells
  rval = mbImpl->get_entities_by_dimension(_fileSet, 2, localCellsOwned);
  ERRORR(rval, "Trouble getting local cells in current file set.");
  assert(!localCellsOwned.empty());

#ifdef USE_MPI
  bool& isParallel = _writeNC->isParallel;
  if (isParallel) {
    ParallelComm*& myPcomm = _writeNC->myPcomm;
    int rank = myPcomm->proc_config().proc_rank();
    int procs = myPcomm->proc_config().proc_size();
    if (procs > 1) {
      unsigned int num_local_verts = localVertsOwned.size();
      rval = myPcomm->filter_pstatus(localVertsOwned, PSTATUS_NOT_OWNED, PSTATUS_NOT);
      ERRORR(rval, "Trouble getting owned vertices in set.");

      // Assume that PARALLEL_RESOLVE_SHARED_ENTS option is set
      // Verify that not all local vertices are owned by the last processor
      if (procs - 1 == rank)
        assert("PARALLEL_RESOLVE_SHARED_ENTS option is set" && localVertsOwned.size() < num_local_verts);

      if (!localEdgesOwned.empty()) {
        rval = myPcomm->filter_pstatus(localEdgesOwned, PSTATUS_NOT_OWNED, PSTATUS_NOT);
        ERRORR(rval, "Trouble getting owned edges in set.");
      }

      rval = myPcomm->filter_pstatus(localCellsOwned, PSTATUS_NOT_OWNED, PSTATUS_NOT);
      ERRORR(rval, "Trouble getting owned cells in set.");
    }
  }
#endif

  std::vector<int> gids(localVertsOwned.size());
  rval = mbImpl->tag_get_data(mGlobalIdTag, localVertsOwned, &gids[0]);
  ERRORR(rval, "Trouble getting global IDs on local vertices.");

  // Get localGidVertsOwned
  std::copy(gids.rbegin(), gids.rend(), range_inserter(localGidVertsOwned));

  if (!localEdgesOwned.empty()) {
    gids.resize(localEdgesOwned.size());
    rval = mbImpl->tag_get_data(mGlobalIdTag, localEdgesOwned, &gids[0]);
    ERRORR(rval, "Trouble getting global IDs on local edges.");

    // Get localGidEdgesOwned
    std::copy(gids.rbegin(), gids.rend(), range_inserter(localGidEdgesOwned));
  }

  gids.resize(localCellsOwned.size());
  rval = mbImpl->tag_get_data(mGlobalIdTag, localCellsOwned, &gids[0]);
  ERRORR(rval, "Trouble getting global IDs on local cells.");

  // Get localGidCellsOwned
  std::copy(gids.rbegin(), gids.rend(), range_inserter(localGidCellsOwned));

  return MB_SUCCESS;
}

ErrorCode NCWriteGCRM::collect_variable_data(std::vector<std::string>& var_names, std::vector<int>& tstep_nums)
{
  NCWriteHelper::collect_variable_data(var_names, tstep_nums);

  std::vector<std::string>& dimNames = _writeNC->dimNames;
  std::vector<int>& dimLens = _writeNC->dimLens;

  // Dimension numbers for other optional levels
  std::vector<unsigned int> opt_lev_dims;

  unsigned int lev_idx;
  std::vector<std::string>::iterator vecIt;

  // Get number of vertex levels P1
  if ((vecIt = std::find(dimNames.begin(), dimNames.end(), "nVertLevelsP1")) != dimNames.end()) {
    lev_idx = vecIt - dimNames.begin();
    opt_lev_dims.push_back(lev_idx);
  }

  // Get number of vertex levels P2
  if ((vecIt = std::find(dimNames.begin(), dimNames.end(), "nVertLevelsP2")) != dimNames.end()) {
    lev_idx = vecIt - dimNames.begin();
    opt_lev_dims.push_back(lev_idx);
  }

  // Get number of soil levels
  if ((vecIt = std::find(dimNames.begin(), dimNames.end(), "nSoilLevels")) != dimNames.end()) {
    lev_idx = vecIt - dimNames.begin();
    opt_lev_dims.push_back(lev_idx);
  }

  std::map<std::string, WriteNC::VarData>& varInfo = _writeNC->varInfo;

  for (size_t i = 0; i < var_names.size(); i++) {
    std::string varname = var_names[i];
    std::map<std::string, WriteNC::VarData>::iterator vit = varInfo.find(varname);
    if (vit == varInfo.end())
      ERRORR(MB_FAILURE, "Can't find one variable.");

    WriteNC::VarData& currentVarData = vit->second;

    // Skip edge variables, if there are no edges
    if (localEdgesOwned.empty() && currentVarData.entLoc == WriteNC::ENTLOCEDGE)
      continue;

    // If nVertLevels dimension is not found, try other optional levels such as nVertLevelsP1
    std::vector<int>& varDims = currentVarData.varDims;
    if (std::find(varDims.begin(), varDims.end(), levDim) == varDims.end()) {
      for (unsigned int j = 0; j < opt_lev_dims.size(); j++) {
        if (std::find(varDims.begin(), varDims.end(), opt_lev_dims[j]) != varDims.end()) {
          currentVarData.numLev = dimLens[opt_lev_dims[j]];
          break;
        }
      }
    }

    if (currentVarData.has_tsteps) {
      // Support non-set variables with 3 dimensions like (Time, nCells, nVertLevels), or
      // 2 dimensions like (Time, nCells)
      assert(3 == varDims.size() || 2 == varDims.size());

      // Time should be the first dimension
      assert(tDim == varDims[0]);

      // Set up writeStarts and writeCounts
      currentVarData.writeStarts.resize(3);
      currentVarData.writeCounts.resize(3);

      // First: Time
      currentVarData.writeStarts[0] = 0; // This value is timestep dependent, will be set later
      currentVarData.writeCounts[0] = 1;

      // Next: nVertices / nCells / nEdges
      switch (currentVarData.entLoc) {
        case WriteNC::ENTLOCVERT:
          // Vertices
          // Start from the first localGidVerts
          // Actually, this will be reset later for writing
          currentVarData.writeStarts[1] = localGidVertsOwned[0] - 1;
          currentVarData.writeCounts[1] = localGidVertsOwned.size();
          break;
        case WriteNC::ENTLOCFACE:
          // Faces
          // Start from the first localGidCells
          // Actually, this will be reset later for writing
          currentVarData.writeStarts[1] = localGidCellsOwned[0] - 1;
          currentVarData.writeCounts[1] = localGidCellsOwned.size();
          break;
        case WriteNC::ENTLOCEDGE:
          // Edges
          // Start from the first localGidEdges
          // Actually, this will be reset later for writing
          currentVarData.writeStarts[1] = localGidEdgesOwned[0] - 1;
          currentVarData.writeCounts[1] = localGidEdgesOwned.size();
          break;
        default:
          ERRORR(MB_FAILURE, "Unexpected entity location type for GCRM non-set variable.");
      }

      // Finally: nVertLevels or other optional levels, it is possible that there
      // is no level dimension for this non-set variable
      currentVarData.writeStarts[2] = 0;
      currentVarData.writeCounts[2] = currentVarData.numLev;
    }

    // Get variable size
    currentVarData.sz = 1;
    for (std::size_t idx = 0; idx != currentVarData.writeCounts.size(); idx++)
      currentVarData.sz *= currentVarData.writeCounts[idx];
  }

  return MB_SUCCESS;
}

ErrorCode NCWriteGCRM::write_nonset_variables(std::vector<WriteNC::VarData>& vdatas, std::vector<int>& tstep_nums)
{
  Interface*& mbImpl = _writeNC->mbImpl;

  int success;
  Range* pLocalEntsOwned = NULL;
  Range* pLocalGidEntsOwned = NULL;

  // Now look at requested var_names; if they have time, we will have a list, and write one at a time
  // For each variable tag in the indexed lists, write a time step data
  // Assume the first dimension is time (need to check); if not, just write regularly
  for (unsigned int i = 0; i < vdatas.size(); i++) {
    WriteNC::VarData& variableData = vdatas[i];

    // Skip edge variables, if there are no edges
    if (localEdgesOwned.empty() && variableData.entLoc == WriteNC::ENTLOCEDGE)
      continue;

    // Time should be the first dimension
    assert(tDim == variableData.varDims[0]);

    // Get local owned entities of this variable
    switch (variableData.entLoc) {
      case WriteNC::ENTLOCVERT:
        // Vertices
        pLocalEntsOwned = &localVertsOwned;
        pLocalGidEntsOwned = &localGidVertsOwned;
        break;
      case WriteNC::ENTLOCEDGE:
        // Edges
        pLocalEntsOwned = &localEdgesOwned;
        pLocalGidEntsOwned = &localGidEdgesOwned;
        break;
      case WriteNC::ENTLOCFACE:
        // Cells
        pLocalEntsOwned = &localCellsOwned;
        pLocalGidEntsOwned = &localGidCellsOwned;
        break;
      default:
        ERRORR(MB_FAILURE, "Unexpected entity location type for MPAS non-set variable.");
    }

    // A typical variable has 3 dimensions as (Time, nCells, nVertLevels)
    for (unsigned int t = 0; t < tstep_nums.size(); t++) {
      // We will write one time step, and count will be one; start will be different
      // Use tag_get_data instead of tag_iterate to get values, as localEntsOwned
      // might not be contiguous.
      variableData.writeStarts[0] = t; // This is time, again
      std::vector<double> tag_data(pLocalEntsOwned->size() * variableData.numLev);
      ErrorCode rval = mbImpl->tag_get_data(variableData.varTags[t], *pLocalEntsOwned, &tag_data[0]);
      ERRORR(rval, "Trouble getting tag data on owned vertices.");

#ifdef PNETCDF_FILE
      size_t nb_writes = pLocalGidEntsOwned->psize();
      std::vector<int> requests(nb_writes), statuss(nb_writes);
      size_t idxReq = 0;
#endif

      // Now write from memory directly
      switch (variableData.varDataType) {
        case NC_DOUBLE: {
          size_t indexInDoubleArray = 0;
          size_t ic = 0;
          for (Range::pair_iterator pair_iter = pLocalGidEntsOwned->pair_begin();
              pair_iter != pLocalGidEntsOwned->pair_end(); ++pair_iter, ic++) {
            EntityHandle starth = pair_iter->first;
            EntityHandle endh = pair_iter->second;
            variableData.writeStarts[1] = (NCDF_SIZE)(starth - 1);
            variableData.writeCounts[1] = (NCDF_SIZE)(endh - starth + 1);

            // Do a partial write, in each subrange
#ifdef PNETCDF_FILE
            // Wait outside this loop
            success = NCFUNCREQP(_vara_double)(_fileId, variableData.varId,
                &(variableData.writeStarts[0]), &(variableData.writeCounts[0]),
                           &(tag_data[indexInDoubleArray]), &requests[idxReq++]);
#else
            success = NCFUNCAP(_vara_double)(_fileId, variableData.varId,
                &(variableData.writeStarts[0]), &(variableData.writeCounts[0]),
                           &(tag_data[indexInDoubleArray]));
#endif
            ERRORS(success, "Failed to read double data in loop");
            // We need to increment the index in double array for the
            // next subrange
            indexInDoubleArray += (endh - starth + 1) * variableData.numLev;
          }
          assert(ic == pLocalGidEntsOwned->psize());
#ifdef PNETCDF_FILE
          success = ncmpi_wait_all(_fileId, requests.size(), &requests[0], &statuss[0]);
          ERRORS(success, "Failed on wait_all.");
#endif
          break;
        }
        default:
          ERRORR(MB_NOT_IMPLEMENTED, "Writing with current data type not implemented yet.");
      }
    }
  }

  return MB_SUCCESS;
}

} /* namespace moab */
