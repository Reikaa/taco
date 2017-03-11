#include "tensor_base.h"

#include <sstream>

#include "tensor.h"
#include "var.h"
#include "storage/storage.h"
#include "format.h"
#include "ir/ir.h"
#include "lower/lower.h"
#include "lower/iteration_schedule.h"
#include "backends/module.h"
#include "util/strings.h"

using namespace std;
using namespace taco::ir;
using namespace taco::storage;

namespace taco {

// These are defined here to separate out the code here
// from the actual storage in PackedTensor
typedef int                     IndexType;
typedef std::vector<IndexType>  IndexArray; // Index values
typedef std::vector<IndexArray> Index;      // [0,2] index arrays per Index
typedef std::vector<Index>      Indices;    // One Index per level

struct TensorBase::Content {
  string                   name;
  vector<int>              dimensions;
  ComponentType            ctype;

  std::vector<Coordinate>  coordinates;

  Format                   format;
  storage::Storage         storage;

  vector<taco::Var>        indexVars;
  taco::Expr               expr;
  vector<void*>            arguments;

  size_t                   allocSize;

  lower::IterationSchedule schedule;
  Stmt                     assembleFunc;
  Stmt                     computeFunc;
  shared_ptr<Module>       module;
};

TensorBase::TensorBase() : content() {
}

TensorBase::TensorBase(ComponentType ctype)
    : TensorBase(util::uniqueName('A'), ctype) {
}

TensorBase::TensorBase(std::string name, ComponentType ctype)
    : TensorBase(name, ctype, {}, Format(), 1)  {
}

TensorBase::TensorBase(ComponentType ctype, vector<int> dimensions,
                       Format format, size_t allocSize)
    : TensorBase(util::uniqueName('A'), ctype, dimensions, format, allocSize) {
}

TensorBase::TensorBase(string name, ComponentType ctype, vector<int> dimensions,
                       Format format, size_t allocSize) : content(new Content) {
  uassert(format.getLevels().size() == dimensions.size())
      << "The number of format levels (" << format.getLevels().size()
      << ") must match the tensor order (" << dimensions.size() << ")";
  content->name = name;
  content->dimensions = dimensions;

  content->storage = Storage(format);
  // Initialize dense storage dimensions
  vector<Level> levels = format.getLevels();
  for (size_t i=0; i < levels.size(); ++i) {
    auto& levelIndex = content->storage.getLevelIndex(i);
    if (levels[i].getType() == LevelType::Dense) {
      levelIndex.ptr = (int*)malloc(sizeof(int));
      levelIndex.ptr[0] = dimensions[i];
    }
  }

  content->ctype = ctype;
  content->allocSize = allocSize;
  
  content->module = make_shared<Module>();
}

string TensorBase::getName() const {
  return content->name;
}

size_t TensorBase::getOrder() const {
  return content->dimensions.size();
}

const vector<int>& TensorBase::getDimensions() const {
  return content->dimensions;
}

const Format& TensorBase::getFormat() const {
  return content->storage.getFormat();
}

const ComponentType& TensorBase::getComponentType() const {
  return content->ctype;
}

const vector<taco::Var>& TensorBase::getIndexVars() const {
  return content->indexVars;
}

const taco::Expr& TensorBase::getExpr() const {
  return content->expr;
}

const storage::Storage& TensorBase::getStorage() const {
  return content->storage;
}

size_t TensorBase::getAllocSize() const {
  return content->allocSize;
}

/// Count unique entries between iterators (assumes values are sorted)
static vector<size_t> getUniqueEntries(const vector<int>::const_iterator& begin,
                                       const vector<int>::const_iterator& end) {
  vector<size_t> uniqueEntries;
  if (begin != end) {
    size_t curr = *begin;
    uniqueEntries.push_back(curr);
    for (auto it = begin+1; it != end; ++it) {
      size_t next = *it;
      iassert(next >= curr);
      if (curr < next) {
        curr = next;
        uniqueEntries.push_back(curr);
      }
    }
  }
  return uniqueEntries;
}

static void packTensor(const vector<int>& dims,
                       const vector<vector<int>>& coords,
                       const double* vals,
                       size_t begin, size_t end,
                       const vector<Level>& levels, size_t i,
                       Indices* indices,
                       vector<double>* values) {

  // Base case: no more tree levels so we pack values
  if (i == levels.size()) {
    if (begin < end) {
      values->push_back(vals[begin]);
    }
    else {
      values->push_back(0.0);
    }
    return;
  }

  auto& level       = levels[i];
  auto& levelCoords = coords[i];
  auto& index       = (*indices)[i];

  switch (level.getType()) {
    case Dense: {
      // Iterate over each index value and recursively pack it's segment
      size_t cbegin = begin;
      for (int j=0; j < (int)dims[i]; ++j) {
        // Scan to find segment range of children
        size_t cend = cbegin;
        while (cend < end && levelCoords[cend] == j) {
          cend++;
        }
        packTensor(dims, coords, vals, cbegin, cend, levels, i+1,
                   indices, values);
        cbegin = cend;
      }
      break;
    }
    case Sparse: {
      auto indexValues = getUniqueEntries(levelCoords.begin()+begin,
                                          levelCoords.begin()+end);

      // Store segment end: the size of the stored segment is the number of
      // unique values in the coordinate list
      index[0].push_back((int)(index[1].size() + indexValues.size()));

      // Store unique index values for this segment
      index[1].insert(index[1].end(), indexValues.begin(), indexValues.end());

      // Iterate over each index value and recursively pack it's segment
      size_t cbegin = begin;
      for (size_t j : indexValues) {
        // Scan to find segment range of children
        size_t cend = cbegin;
        while (cend < end && levelCoords[cend] == (int)j) {
          cend++;
        }
        packTensor(dims, coords, vals, cbegin, cend, levels, i+1,
                   indices, values);
        cbegin = cend;
      }
      break;
    }
    case Fixed: {
      int fixedValue = index[0][0];
      auto indexValues = levelCoords;

      // Store segment end: the size of the stored segment is the number of
      // unique values in the coordinate list
      int segmentSize = end - begin ;
      // Store unique index values for this segment
      size_t cbegin = begin;
      if (segmentSize > 0) {
        index[1].insert(index[1].end(), indexValues.begin()+begin,
                        indexValues.begin()+begin+segmentSize);
        for (int j=0; j<segmentSize; j++) {
          // Scan to find segment range of children
          size_t cend = cbegin+segmentSize;
          while (cend < end && levelCoords[cend] == (int)j) {
            cend++;
          }
          packTensor(dims, coords, vals, cbegin, cend, levels, i+1,
                     indices, values);
          cbegin++;
        }
      }
      // Complete index if necessary with the last index value
      auto curSize=segmentSize;
      while (curSize < fixedValue) {
        if (segmentSize>0)
          index[1].insert(index[1].end(),indexValues[segmentSize-1]);
        else
          index[1].insert(index[1].end(),0);
        packTensor(dims, coords, vals, cbegin, cbegin, levels, i+1,
                   indices, values);
        curSize++;
      }
      break;
    }
    case Offset:
    case Replicated: {
      not_supported_yet;
      break;
    }
  }
}

static int findMaxFixedValue(const vector<vector<int>>& coords,
                             const vector<Level>& levels, size_t i,
                             size_t numCoords) {
  if (i == levels.size()-1) {
    return numCoords;
  }

  // Find maxs for level i
  size_t maxSize=0;
  int maxCoord=coords[i][0];
  int coordCur=maxCoord;
  size_t sizeCur=1;
  for (size_t j=1; j<numCoords; j++) {
    if (coords[i][j] == coordCur) {
      sizeCur++;
    }
    else {
      coordCur=coords[i][j];
      if (sizeCur > maxSize) {
        maxSize = sizeCur;
        maxCoord = coordCur;
      }
      sizeCur=1;
    }
  }
  if (sizeCur > maxSize) {
    maxSize = sizeCur;
    maxCoord = coordCur;
  }
  // clean coords for next level
  vector<vector<int>> newCoords(levels.size());
  for (size_t j=1; j<numCoords; j++) {
    if (coords[i][j] == maxCoord) {
      for (size_t k=0; k<levels.size();k++) {
        newCoords[k].push_back(coords[k][j]);
      }
    }
  }
  return findMaxFixedValue(newCoords,levels,i+1,maxSize);
}


void TensorBase::insert(const std::vector<int>& coord, int val) {
  uassert(coord.size() == getOrder()) << "Wrong number of indices";
  uassert(getComponentType() == ComponentType::Int) <<
      "Cannot insert a value of type '" << ComponentType::Int << "' " <<
      "into a tensor with component type " << getComponentType();
  content->coordinates.push_back(Coordinate(coord, val));
}

void TensorBase::insert(const std::vector<int>& coord, float val) {
  uassert(coord.size() == getOrder()) << "Wrong number of indices";
  uassert(getComponentType() == ComponentType::Float) <<
      "Cannot insert a value of type '" << ComponentType::Float << "' " <<
      "into a tensor with component type " << getComponentType();
  content->coordinates.push_back(Coordinate(coord, val));
}

void TensorBase::insert(const std::vector<int>& coord, double val) {
  uassert(coord.size() == getOrder()) << "Wrong number of indices";
  uassert(getComponentType() == ComponentType::Double) <<
      "Cannot insert a value of type '" << ComponentType::Double << "' " <<
      "into a tensor with component type " << getComponentType();
  content->coordinates.push_back(Coordinate(coord, val));
}

void TensorBase::insert(const std::vector<int>& coord, bool val) {
  uassert(coord.size() == getOrder()) << "Wrong number of indices";
  uassert(getComponentType() == ComponentType::Bool) <<
      "Cannot insert a value of type '" << ComponentType::Bool << "' " <<
      "into a tensor with component type " << getComponentType();
  content->coordinates.push_back(Coordinate(coord, val));
}

void TensorBase::setCSR(double* vals, int* rowPtr, int* colIdx) {
  uassert(getFormat().isCSR()) <<
      "setCSR: the tensor " << getName() << " is not defined in the CSR format";
  auto S = getStorage();
  std::vector<int> denseDim = {getDimensions()[0]};
  S.setLevelIndex(0,util::copyToArray(denseDim),nullptr);
  S.setLevelIndex(1, rowPtr, colIdx);
  S.setValues(vals);
}

void TensorBase::getCSR(double** vals, int** rowPtr, int** colIdx) {
  uassert(getFormat().isCSR()) <<
      "getCSR: the tensor " << getName() << " is not defined in the CSR format";
  auto S = getStorage();
  *vals = S.getValues();
  *rowPtr = S.getLevelIndex(1).ptr;
  *colIdx = S.getLevelIndex(1).idx;
}

void TensorBase::setCSC(double* vals, int* colPtr, int* rowIdx) {
  uassert(getFormat().isCSC()) <<
      "setCSC: the tensor " << getName() << " is not defined in the CSC format";
  auto S = getStorage();
  std::vector<int> denseDim = {getDimensions()[1]};
  S.setLevelIndex(0,util::copyToArray(denseDim),nullptr);
  S.setLevelIndex(1, colPtr, rowIdx);
  S.setValues(vals);
}

void TensorBase::getCSC(double** vals, int** colPtr, int** rowIdx) {
  uassert(getFormat().isCSC()) <<
      "getCSC: the tensor " << getName() << " is not defined in the CSC format";

  auto S = getStorage();
  *vals = S.getValues();
  *colPtr = S.getLevelIndex(1).ptr;
  *rowIdx = S.getLevelIndex(1).idx;
}

void TensorBase::read(std::string filename) {
  std::string extension = filename.substr(filename.find_last_of(".") + 1);
  if(extension == "rb") {
    readHB(filename);
  }
  else if (extension == "mtx") {
    readMTX(filename);
  }
  else {
    uerror << "file extension not supported " << filename << std::endl;
  }
}

void TensorBase::readHB(std::string filename) {
  uassert(getFormat().isCSC()) <<
      "readHB: the tensor " << getName() << " is not defined in the CSC format";
  std::ifstream HBfile;

  HBfile.open(filename.c_str());
  uassert(HBfile.is_open())
  << " Error opening the file " << filename.c_str();
  int nrow, ncol;
  int *colptr = NULL;
  int *rowind = NULL;
  double *values = NULL;

  hb::readFile(HBfile, &nrow, &ncol, &colptr, &rowind, &values);
  uassert((nrow==getDimensions()[0]) && (ncol==getDimensions()[1])) <<
      "readHB: the tensor " << getName() <<
      " does not have the same dimension in its declaration and HBFile" <<
  filename.c_str();
  auto S = getStorage();
  std::vector<int> denseDim = {getDimensions()[1]};
  S.setLevelIndex(0,util::copyToArray(denseDim),nullptr);
  S.setLevelIndex(1,colptr,rowind);
  S.setValues(values);

  HBfile.close();
}

void TensorBase::writeHB(std::string filename) const {
  uassert(getFormat().isCSC()) <<
      "writeHB: the tensor " << getName() <<
      " is not defined in the CSC format";
  std::ofstream HBfile;

  HBfile.open(filename.c_str());
  uassert(HBfile.is_open()) << " Error opening the file " << filename.c_str();

  auto S = getStorage();
  auto size = S.getSize();

  double *values = S.getValues();
  int *colptr = S.getLevelIndex(1).ptr;
  int *rowind = S.getLevelIndex(1).idx;
  int nrow = getDimensions()[0];
  int ncol = getDimensions()[1];
  int nnzero = size.values;
  std::string key = getName();
  int valsize = size.values;
  int ptrsize = size.levelIndices[1].ptr;
  int indsize = size.levelIndices[1].idx;

  hb::writeFile(HBfile,const_cast<char*> (key.c_str()),
                nrow,ncol,nnzero,
                ptrsize,indsize,valsize,
                colptr,rowind,values);

  HBfile.close();
}

void TensorBase::readMTX(std::string filename) {
  uassert(getFormat().isCSC()) <<
  "readMTX: the tensor " << getName() <<
  " is not defined in the CSC format";
  std::ifstream MTXfile;

  MTXfile.open(filename.c_str());
  uassert(MTXfile.is_open())
      << " Error opening the file " << filename.c_str() ;

  int nrow,ncol,nnzero;
  mtx::readFile(MTXfile, &nrow, &ncol, &nnzero, this);
  uassert((nrow==getDimensions()[0])&&(ncol==getDimensions()[1])) <<
      "readMTX: the tensor " << getName() <<
      " does not have the same dimension in its declaration and MTXFile" <<
  filename.c_str();

  MTXfile.close();
}

void TensorBase::pack() {
  // Pack the coordinates (stored as structure-of-arrays) into a data structure
  // given by the tensor format.

  // Pack scalar
  if (getOrder() == 0) {
    content->storage.setValues((double*)malloc(sizeof(double)));
    content->storage.getValues()[0] =
        content->coordinates[content->coordinates.size()-1].dval;
    content->coordinates.clear();
    return;
  }

  const std::vector<Level>& levels     = getFormat().getLevels();
  const std::vector<int>&   dimensions = getDimensions();

  iassert(levels.size() == getOrder());

  // Packing code currently only packs coordinates in the order of the
  // dimensions. To work around this we just permute each coordinate according
  // to the storage dimensions.
  std::vector<int> permutation;
  for (auto& level : levels) {
    permutation.push_back((int)level.getDimension());
  }

  std::vector<int> permutedDimensions(getOrder());
  for (size_t i = 0; i < getOrder(); ++i) {
    permutedDimensions[i] = dimensions[permutation[i]];
  }

  std::vector<Coordinate> permutedCoords;
  permutation.reserve(content->coordinates.size());
  for (size_t i=0; i < content->coordinates.size(); ++i) {
    auto& coord = content->coordinates[i];
    std::vector<int> ploc(coord.loc.size());
    for (size_t j=0; j < getOrder(); ++j) {
      ploc[j] = coord.loc[permutation[j]];
    }

    switch (getComponentType().getKind()) {
      case ComponentType::Bool:
        permutedCoords.push_back(Coordinate(ploc, coord.bval));
        break;
      case ComponentType::Int:
        permutedCoords.push_back(Coordinate(ploc, coord.ival));
        break;
      case ComponentType::Float:
        permutedCoords.push_back(Coordinate(ploc, coord.fval));
        break;
      case ComponentType::Double:
        permutedCoords.push_back(Coordinate(ploc, coord.dval));
        break;
      default:
        not_supported_yet;
        break;
    }
  }
  content->coordinates.clear();

  // The pack code requires the coordinates to be sorted
  std::sort(permutedCoords.begin(), permutedCoords.end());

  // convert coords to structure of arrays
  std::vector<std::vector<int>> coords(getOrder());
  for (size_t i=0; i < getOrder(); ++i) {
    coords[i] = std::vector<int>(permutedCoords.size());
  }

  // TODO: element type should not be hard-coded to double
  std::vector<double> vals(permutedCoords.size());

  for (size_t i=0; i < permutedCoords.size(); ++i) {
    for (size_t d=0; d < getOrder(); ++d) {
      coords[d][i] = permutedCoords[i].loc[d];
    }
    switch (getComponentType().getKind()) {
      case ComponentType::Bool:
        vals[i] = permutedCoords[i].bval;
        break;
      case ComponentType::Int:
        vals[i] = permutedCoords[i].ival;
        break;
      case ComponentType::Float:
        vals[i] = permutedCoords[i].fval;
        break;
      case ComponentType::Double:
        vals[i] = permutedCoords[i].dval;
        break;
      default:
        not_supported_yet;
        break;
    }
  }

  iassert(coords.size() > 0);
  size_t numCoords = coords[0].size();

  Indices indices;
  indices.reserve(levels.size());

  // Create the vectors to store pointers to indices/index sizes
  size_t nnz = 1;
  for (size_t i=0; i < levels.size(); ++i) {
    auto& level = levels[i];
    switch (level.getType()) {
      case Dense: {
        indices.push_back({});
        nnz *= permutedDimensions[i];
        break;
      }
      case Sparse: {
        // A sparse level packs nnz down to #coords
        nnz = numCoords;

        // Sparse indices have two arrays: a segment array and an index array
        indices.push_back({{}, {}});

        // Add start of first segment
        indices[i][0].push_back(0);
        break;
      }
      case Fixed: {
        // A fixed level packs nnz down to #coords
        nnz = numCoords;

        // Fixed indices have two arrays: a segment array and an index array
        indices.push_back({{}, {}});

        // Add maximum size to segment array
        size_t maxSize = findMaxFixedValue(coords,levels,0,numCoords);

        indices[i][0].push_back(maxSize);
        break;
      }
      case Offset:
      case Replicated: {
        not_supported_yet;
        break;
      }
    }
  }

  tassert(getComponentType() == ComponentType::Double)
      << "make the packing machinery work with other primitive types later. "
      << "Right now we're specializing to doubles so that we can use a "
      << "resizable vector, but eventually we should use a two pass pack "
      << "algorithm that figures out sizes first, and then packs the data";

  std::vector<double> values;

  // Pack indices and values
  packTensor(permutedDimensions, coords, (const double*)vals.data(), 0, 
             numCoords, levels, 0, &indices, &values);

  // Copy packed data into tensor storage
  for (size_t i=0; i < levels.size(); ++i) {
    LevelType levelType = levels[i].getType();

    int* ptr = nullptr;
    int* idx = nullptr;
    switch (levelType) {
      case LevelType::Dense:
        ptr = util::copyToArray({permutedDimensions[i]});
        idx = nullptr;
        break;
      case LevelType::Sparse:
      case LevelType::Fixed:
        ptr = util::copyToArray(indices[i][0]);
        idx = util::copyToArray(indices[i][1]);
        break;
      case LevelType::Offset:
      case LevelType::Replicated:{
        not_supported_yet;
        break;
      }
    }
    content->storage.setLevelIndex(i, ptr, idx);
  }
  content->storage.setValues(util::copyToArray(values));
}

void TensorBase::compile() {
  iassert(getExpr().defined()) << "No expression defined for tensor";

  content->assembleFunc = lower::lower(*this, "assemble", {lower::Assemble});

  content->computeFunc  = lower::lower(*this, "compute", {lower::Compute});

  content->module->addFunction(content->assembleFunc);
  content->module->addFunction(content->computeFunc);
  content->module->compile();
}

void TensorBase::assemble() {
  content->module->callFunc("assemble", content->arguments.data());
  
  size_t j = 0;
  auto resultStorage = getStorage();
  auto resultFormat = resultStorage.getFormat();
  for (size_t i=0; i<resultFormat.getLevels().size(); i++) {
    Storage::LevelIndex& levelIndex = resultStorage.getLevelIndex(i);
    auto& levelFormat = resultFormat.getLevels()[i];
    switch (levelFormat.getType()) {
      case Dense:
        j++;
        break;
      case Sparse:
        levelIndex.ptr = (int*)content->arguments[j++];
        levelIndex.idx = (int*)content->arguments[j++];
        break;
      case Offset:
      case Fixed:
      case Replicated:
        not_supported_yet;
        break;
    }
  }

  const size_t allocation_size = resultStorage.getSize().values;
  content->arguments[j] = resultStorage.getValues() 
                        = (double*)malloc(allocation_size * sizeof(double));
  // Set values to 0.0 in case we are doing a += operation
  memset(resultStorage.getValues(), 0, allocation_size * sizeof(double));
}

void TensorBase::compute() {
  content->module->callFunc("compute", content->arguments.data());
}

void TensorBase::evaluate() {
  compile();
  assemble();
  compute();
}

static inline vector<void*> packArguments(const TensorBase& tensor) {
  vector<void*> arguments;

  // Pack the result tensor
  auto resultStorage = tensor.getStorage();
  auto resultFormat = resultStorage.getFormat();
  for (size_t i=0; i<resultFormat.getLevels().size(); i++) {
    Storage::LevelIndex levelIndex = resultStorage.getLevelIndex(i);
    auto& levelFormat = resultFormat.getLevels()[i];
    switch (levelFormat.getType()) {
      case Dense:
        arguments.push_back((void*)levelIndex.ptr);
        break;
      case Sparse:
        arguments.push_back((void*)levelIndex.ptr);
        arguments.push_back((void*)levelIndex.idx);
//        arguments.push_back((void*)&levelIndex.ptr);
//        arguments.push_back((void*)&levelIndex.idx);
        break;
      case Offset:
      case Fixed:
      case Replicated:
        not_supported_yet;
        break;
    }
  }
  arguments.push_back((void*)resultStorage.getValues());
//  arguments.push_back((void*)&resultStorage.getValues());

  // Pack operand tensors
  vector<TensorBase> operands = internal::getOperands(tensor.getExpr());
  for (auto& operand : operands) {
    Storage storage = operand.getStorage();
    Format format = storage.getFormat();
    for (size_t i=0; i<format.getLevels().size(); i++) {
      Storage::LevelIndex levelIndex = storage.getLevelIndex(i);
      auto& levelFormat = format.getLevels()[i];
      switch (levelFormat.getType()) {
        case Dense:
          arguments.push_back((void*)levelIndex.ptr);
          break;
        case Sparse:
          arguments.push_back((void*)levelIndex.ptr);
          arguments.push_back((void*)levelIndex.idx);
          break;
        case Offset:
        case Fixed:
        case Replicated:
          not_supported_yet;
          break;
      }
    }
    arguments.push_back((void*)storage.getValues());
  }

  return arguments;
}

void TensorBase::setExpr(taco::Expr expr) {
  content->expr = expr;

  storage::Storage storage = getStorage();
  Format format = storage.getFormat();
  auto& levels = format.getLevels();
  for (size_t i=0; i < levels.size(); ++i) {
    Level level = levels[i];
    auto& levelIndex = storage.getLevelIndex(i);
    switch (level.getType()) {
      case LevelType::Dense:
        break;
      case LevelType::Sparse:
        levelIndex.ptr = (int*)malloc(getAllocSize() * sizeof(int));
        levelIndex.ptr[0] = 0;
        levelIndex.idx = (int*)malloc(getAllocSize() * sizeof(int));
        break;
      case LevelType::Offset:
      case LevelType::Fixed:
      case LevelType::Replicated:
        not_supported_yet;
        break;
    }
  }

  content->arguments = packArguments(*this);
}

void TensorBase::setIndexVars(vector<taco::Var> indexVars) {
  content->indexVars = indexVars;
}

void TensorBase::printIterationSpace() const {
  for (auto& operand : internal::getOperands(getExpr())) {
    std::cout << operand << std::endl;
  }

  string funcName = "print";
  auto print = lower::lower(*this, funcName, {lower::Print});
  std::cout << std::endl << "# IR:" << std::endl;
  std::cout << print << std::endl;

  content->module = make_shared<Module>();
  content->module->addFunction(print);
  content->module->compile();

  std::cout << std::endl << "# Code" << std::endl
            << content->module->getSource();
  std::cout << std::endl << "# Output:" << std::endl;
  content->module->callFunc(funcName, content->arguments.data());

  std::cout << std::endl << "# Result index:" << std::endl;
  std::cout << getStorage() << std::endl;
}

void TensorBase::printIR(std::ostream& os) const {
  bool printed = false;
  if (content->assembleFunc != nullptr) {
    os << "# Assembly IR" << endl;
    printAssemblyIR(os, false);
    printed = true;
  }
  if (content->computeFunc != nullptr) {
    if (printed == true) os << endl;
    os << "# Compute IR" << endl;
    printComputeIR(os, false);
    printed = true;
  }

  os << std::endl << "# Result index:" << std::endl;
  os << getStorage() << std::endl;
}

void TensorBase::printComputeIR(std::ostream& os, bool color) const {
  IRPrinter printer(os,color);
  content->computeFunc.as<Function>()->body.accept(&printer);
}

void TensorBase::printAssemblyIR(std::ostream& os, bool color) const {
  IRPrinter printer(os,color);
  content->assembleFunc.as<Function>()->body.accept(&printer);
}

bool operator!=(const TensorBase& l, const TensorBase& r) {
  return l.content != r.content;
}

bool operator<(const TensorBase& l, const TensorBase& r) {
  return l.content < r.content;
}

ostream& operator<<(ostream& os, const TensorBase& t) {
  vector<string> dimStrings;
  for (int dim : t.getDimensions()) {
    dimStrings.push_back(to_string(dim));
  }
  os << t.getName()
     << " (" << util::join(dimStrings, "x") << ", " << t.getFormat() << ")";

  if (t.content->coordinates.size() > 0) {
    os << std::endl << "Coordinates: ";
    for (auto& coord : t.content->coordinates) {
      os << std::endl << "  (" << util::join(coord.loc) << "): ";
      switch (t.getComponentType().getKind()) {
        case ComponentType::Bool:
          os << coord.bval;
          break;
        case ComponentType::Int:
          os << coord.ival;
          break;
        case ComponentType::Float:
          os << coord.fval;
          break;
        case ComponentType::Double:
          os << coord.dval;
          break;
        default:
          not_supported_yet;
          break;
      }
    }
  } else if (t.getStorage().defined()) {
    // Print packed data
    os << endl << t.getStorage();
  }

  return os;
}

}