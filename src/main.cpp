#include <cstdio>
#include <cstdlib>
#include "sqlite3ext.h"

SQLITE_EXTENSION_INIT1

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>


#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/index_factory.h>
#include <faiss/index_io.h>
#include <faiss/utils/utils.h>
#include <faiss/impl/io.h>
#include <faiss/impl/AuxIndexStructures.h>

#include "vectors.h"

#pragma region work 

void del(void*p) {
  //delete p;
}
static void vss_version(sqlite3_context *context, int argc, sqlite3_value **argv) {
  sqlite3_result_text(context, "", -1, SQLITE_STATIC);
}
static void vss_debug(sqlite3_context *context, int argc, sqlite3_value **argv) {
  const char * debug = sqlite3_mprintf("faiss version: %d.%d.%d\nfaiss compile options: %s", 
  FAISS_VERSION_MAJOR, FAISS_VERSION_MINOR, FAISS_VERSION_PATCH,
  faiss::get_compile_options().c_str());
  sqlite3_result_text(context, debug, -1, SQLITE_TRANSIENT);
  sqlite3_free((void *) debug);
}


#pragma endregion

#pragma region vtab

// https://github.com/sqlite/sqlite/blob/master/src/json.c#L88-L89
#define JSON_SUBTYPE  74    /* Ascii for "J" */

#include <nlohmann/json.hpp>
using json = nlohmann::json;

static std::vector<float>* valueAsVector(sqlite3_value*value) {
    VectorFloat* v = (VectorFloat*) sqlite3_value_pointer(value, VECTOR_FLOAT_POINTER_NAME);
    if (v!=NULL) return new std::vector<float>(v->data, v->data + v->size);

    if(sqlite3_value_subtype(value) == JSON_SUBTYPE) {
      std::vector<float> v; 
      json data = json::parse(sqlite3_value_text(value));
      data.get_to(v);
      return  new std::vector<float>(v);
    }
    return NULL;
    
}


struct VssSearchParams {
  std::vector<float> * vector;
  sqlite3_int64 k;
};

static void VssSearchParamsFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  std::vector<float> * vector = valueAsVector(argv[0]);
  sqlite3_int64 k = sqlite3_value_int64(argv[1]);
  VssSearchParams* params = new VssSearchParams();
  params->vector = vector;
  params->k = k;
  sqlite3_result_pointer(context, params, "vss0_searchparams", 0);
}

struct VssRangeSearchParams {
  std::vector<float> * vector;
  float distance;
};

static void VssRangeSearchParamsFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  std::vector<float> * vector = valueAsVector(argv[0]);
  float distance = sqlite3_value_double(argv[1]);
  VssRangeSearchParams* params = new VssRangeSearchParams();
  params->vector = vector;
  params->distance = distance;
  sqlite3_result_pointer(context, params, "vss0_rangesearchparams", 0);
}

static int write_index_insert(faiss::Index * index, sqlite3*db, char * name, int i) {
  faiss::VectorIOWriter * w = new faiss::VectorIOWriter();
  faiss::write_index(index, w);
  sqlite3_int64 nIn = w->data.size();

  sqlite3_stmt *stmt;
  char * q = sqlite3_mprintf("insert into \"%w_index\"(c%d) values (?)", name, i);
  int rc = sqlite3_prepare_v2(db, q, -1, &stmt, 0);
  if (rc != SQLITE_OK || stmt==0) {
    printf("error prepping stmt\n");
    return SQLITE_ERROR;
  }
  sqlite3_bind_blob64(stmt, 1, w->data.data(), nIn, sqlite3_free);
  if(sqlite3_step(stmt) != SQLITE_DONE) {
    printf("error inserting?\n");
    sqlite3_finalize(stmt);
    return SQLITE_ERROR;
  }
  sqlite3_free(q);
  sqlite3_finalize(stmt);
  return SQLITE_OK;
}


static int shadow_data_insert(sqlite3*db, char * name, sqlite3_int64 *rowid, sqlite3_int64 *retRowid) {
  sqlite3_stmt *stmt;
  sqlite3_str *query = sqlite3_str_new(0);
  if(rowid == NULL) {
    sqlite3_str_appendf(query, "insert into \"%w_data\"(x) values (?)", name);
    char * q = sqlite3_str_finish(query);
    int rc = sqlite3_prepare_v2(db, q, -1, &stmt, 0);
    if (rc != SQLITE_OK || stmt==0) {
      printf("error prepping stmt\n");
      return SQLITE_ERROR;
    }
    sqlite3_bind_null(stmt, 1);
    if(sqlite3_step(stmt) != SQLITE_DONE) {
      printf("error inserting?\n");
      sqlite3_finalize(stmt);
      return SQLITE_ERROR;
    }
    sqlite3_free(q);
  } else {
    sqlite3_str_appendf(query, "insert into \"%w_data\"(rowid, x) values (?, ?) returning rowid;", name);
    char * q = sqlite3_str_finish(query);
    int rc = sqlite3_prepare_v2(db, q, -1, &stmt, 0);
    if (rc != SQLITE_OK || stmt==0) {
      printf("error prepping stmt\n");
      return SQLITE_ERROR;
    }
    sqlite3_bind_int64(stmt, 1, *rowid);
    sqlite3_bind_null(stmt, 2);
    if(sqlite3_step(stmt) != SQLITE_ROW) {
      printf("error inserting, no row stepped? %s\n", sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      return SQLITE_ERROR;
    }
    *retRowid = sqlite3_column_int64(stmt, 0);
    sqlite3_free(q);
  }
  sqlite3_finalize(stmt);
  return SQLITE_OK;
}

static faiss::Index * read_index_select(sqlite3 * db, const char * name, int i) {
  sqlite3_stmt *stmt;
  char * q = sqlite3_mprintf("select c%d from \"%w_index\" where c%d is not null", i, name, i);
  int rc = sqlite3_prepare_v2(db, q, -1, &stmt, 0);
  if (rc != SQLITE_OK || stmt==0) {
    printf("error prepping stmt: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    return 0;
  }
  if(sqlite3_step(stmt) != SQLITE_ROW) {
    printf("connect no row??\n");
    sqlite3_finalize(stmt);
    return 0;
  }
  const void * index_data = sqlite3_column_blob(stmt, 0);
  int64_t n = sqlite3_column_bytes(stmt, 0);
  faiss::VectorIOReader * r = new faiss::VectorIOReader();
  std::copy((const uint8_t*) index_data, ((const uint8_t*)index_data) + n, std::back_inserter(r->data));
  sqlite3_free(q);
  sqlite3_finalize(stmt);
  return faiss::read_index(r);
}

static int create_shadow_tables(sqlite3 * db, const char * schema, const char * name, int n) {
  sqlite3_str *create_index_str = sqlite3_str_new(db);
  sqlite3_str_appendf(create_index_str, "CREATE TABLE \"%w\".\"%w_index\"(", schema, name);
  for(int i = 0; i < n; i++) {
    const char * format;
    if(i==0) {
      format = "c%d";
    }else {
      format = ", c%d";
    }
    sqlite3_str_appendf(create_index_str, format, i);
  }
  sqlite3_str_appendall(create_index_str, ")");
  const char * zCreateIndex = sqlite3_str_finish(create_index_str);
  int rc = sqlite3_exec(db, zCreateIndex, 0, 0, 0);  
  sqlite3_free((void *) zCreateIndex);
  if (rc != SQLITE_OK) return rc;

  const char * zCreateData = sqlite3_mprintf("CREATE TABLE \"%w\".\"%w_data\"(x);", schema, name);
  rc = sqlite3_exec(db, zCreateData, 0, 0, 0);
  sqlite3_free((void *) zCreateData);
  return rc;
  
}

static int drop_shadow_tables(sqlite3*db, char * name) {
  const char* drops[2] = {"drop table \"%w_index\";", "drop table \"%w_data\";"};
  for(int i = 0; i < 2; i++) {
    const char * s = drops[i];
    
    sqlite3_stmt *stmt;
    
    sqlite3_str *query = sqlite3_str_new(0);
    sqlite3_str_appendf(query, s, name);
    char * q = sqlite3_str_finish(query);

    int rc = sqlite3_prepare_v2(db, q, -1, &stmt, 0);
    if (rc != SQLITE_OK || stmt==0) {
      printf("error prepping stmt\n");
      return SQLITE_ERROR;
    }
    if(sqlite3_step(stmt) != SQLITE_DONE) {
      printf("error dropping?\n");
      sqlite3_finalize(stmt);
      return SQLITE_ERROR;
    }
    sqlite3_free(q);
    sqlite3_finalize(stmt);
    
  }
  return SQLITE_OK;
}

#define VSS_SEARCH_FUNCTION       SQLITE_INDEX_CONSTRAINT_FUNCTION
#define VSS_RANGE_SEARCH_FUNCTION SQLITE_INDEX_CONSTRAINT_FUNCTION + 1

typedef struct vss_index_vtab vss_index_vtab;
struct vss_index_vtab {
  sqlite3_vtab base;  /* Base class - must be first */
  sqlite3 * db;
  char * name;
  sqlite3_int64 indexCount;
  std::vector<faiss::Index *> *indexes;
  std::vector<std::vector<float>> * trainings;
  bool isTraining;
  bool isInsertData;
};

enum QueryType {search, range_search, fullscan}; 

typedef struct vss_index_cursor vss_index_cursor;
struct vss_index_cursor {
  sqlite3_vtab_cursor base;  /* Base class - must be first */
  vss_index_vtab * table;

  sqlite3_int64 iCurrent;
  sqlite3_int64 iRowid;

  QueryType query_type;

  // for query_type == QueryType::search
  sqlite3_int64 k;
  std::vector<faiss::idx_t> *nns;
  std::vector<float> *dis;
  
  // for query_type == QueryType::range_search
  faiss::RangeSearchResult * range_search_result;

  // for query_type == QueryType::fullscan
  sqlite3_stmt * stmt;
  int step_result;
};

struct VssIndexColumn {
  std::string name;
  sqlite3_int64 dimensions;
  std::string factory;
};

std::vector<VssIndexColumn>* parse_constructor(int argc, const char *const*argv) {
  std::vector<VssIndexColumn>* columns = new std::vector<VssIndexColumn>();
  for(int i = 3; i < argc; i++) {
    std::string arg = std::string(argv[i]);

    std::size_t lparen = arg.find("(");
    std::size_t rparen = arg.find(")");
    if(lparen==std::string::npos || rparen==std::string::npos || lparen >= rparen) {
      return NULL;
    }
    std::string name = arg.substr(0, lparen);
    std::string sDimensions = arg.substr(lparen+1, rparen - lparen-1);
    //= std::string("x");
    sqlite3_int64 dimensions = std::atoi(sDimensions.c_str());

    std::size_t lquote = arg.find("\"");
    std::size_t rquote = arg.find_last_of("\"");
    if(lquote==std::string::npos || rquote==std::string::npos || lquote >= rquote) {
      return NULL;
    }
    std::string factory = arg.substr(lquote+1, rquote - lquote-1);
    columns->push_back(VssIndexColumn{name, dimensions, factory});
  }
  // TODO
  return columns;
}

static int init(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr,
  bool isCreate
) {
  sqlite3_vtab_config(db, SQLITE_VTAB_CONSTRAINT_SUPPORT, 1);
  int rc;

  sqlite3_str *str = sqlite3_str_new(NULL);
  sqlite3_str_appendall(str, "CREATE TABLE x(distance hidden, operation hidden");
  std::vector<VssIndexColumn> * columns = parse_constructor(argc, argv);
  for (auto column = columns->begin(); column != columns->end(); ++column) {
    sqlite3_str_appendf(str, ", \"%w\"", column->name.c_str());
  }
  sqlite3_str_appendall(str, ")");
  const char * query = sqlite3_str_finish(str);
  rc = sqlite3_declare_vtab(db, query);
  sqlite3_free((void*) query);
  
  #define VSS_INDEX_COLUMN_DISTANCE    0
  #define VSS_INDEX_COLUMN_OPERATION   1
  #define VSS_INDEX_COLUMN_VECTORS     2
  
  if( rc!=SQLITE_OK ) return 0;
  
  vss_index_vtab *pNew;
  pNew = (vss_index_vtab *) sqlite3_malloc( sizeof(*pNew) );
  *ppVtab = (sqlite3_vtab*)pNew;
  memset(pNew, 0, sizeof(*pNew));
  //return SQLITE_ERROR;
  pNew->indexCount = columns->size();
  pNew->indexes = new std::vector<faiss::Index *> ();
  
  if (isCreate) {
    create_shadow_tables(db, argv[1], argv[2], pNew->indexCount);
    int i;
    for (auto column = columns->begin(); column != columns->end(); ++column) {
      faiss::Index *index = faiss::index_factory(column->dimensions, column->factory.c_str());
      pNew->indexes->push_back(index);
    }
  } else {
    for(int i = 0; i < pNew->indexCount; i++) {
      pNew->indexes->push_back(read_index_select(db, argv[2], i));
    }
  }

  pNew->trainings = new std::vector<std::vector<float>>();
  
  pNew->isTraining = false;
  pNew->isInsertData = false;
  pNew->name = sqlite3_mprintf("%s", argv[2]);
  pNew->db = db;
    
  return SQLITE_OK;
}

static int vssIndexCreate(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  return init(db, pAux, argc, argv, ppVtab, pzErr, true);
}

static int vssIndexConnect(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  return init(db, pAux, argc, argv, ppVtab, pzErr, false);
}

static int vssIndexDisconnect(sqlite3_vtab *pVtab){
  vss_index_vtab *p = (vss_index_vtab*)pVtab;
  sqlite3_free(p);
  return SQLITE_OK;
}

static int vssIndexDestroy(sqlite3_vtab *pVtab){
  vss_index_vtab *p = (vss_index_vtab*)pVtab;
  drop_shadow_tables(p->db, p->name);
  sqlite3_free(p);
  return SQLITE_OK;
}

static int vssIndexOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor){
  vss_index_cursor *pCur;
  vss_index_vtab *p = (vss_index_vtab*)pVtab;
  pCur = (vss_index_cursor *) sqlite3_malloc( sizeof(*pCur) );
  if( pCur==0 ) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));

  *ppCursor = &pCur->base;
  pCur->table = p;
  //pCur->nns = new std::vector<faiss::idx_t>(5);
  //pCur->dis = new std::vector<float>(5);
  
  return SQLITE_OK;
}

static int vssIndexClose(sqlite3_vtab_cursor *cur){
  vss_index_cursor *pCur = (vss_index_cursor*)cur;
  if(pCur->stmt) sqlite3_finalize(pCur->stmt);
  sqlite3_free(pCur);
  return SQLITE_OK;
}

static int vssIndexBestIndex(
  sqlite3_vtab *tab,
  sqlite3_index_info *pIdxInfo
){
  int iSearchTerm = -1;
  int iRangeSearchTerm = -1;
  int iXSearchColumn = -1;
  //printf("best index, %d\n", pIdxInfo->nConstraint);

  for(int i = 0; i < pIdxInfo->nConstraint; i++) {
    auto constraint = pIdxInfo->aConstraint[i];
    //printf("\t[%d] col=%d, op=%d", i, pIdxInfo->aConstraint[i].iColumn, pIdxInfo->aConstraint[i].op);
    if(sqlite3_vtab_in(pIdxInfo, i, -1)) {
      //printf(" [IN POSSIBLE]");
    }
    //printf("\n");


    if(!constraint.usable) continue;
    if(constraint.op == VSS_SEARCH_FUNCTION) {
      iSearchTerm = i;
      iXSearchColumn = constraint.iColumn;
    }
    if(constraint.op == VSS_RANGE_SEARCH_FUNCTION) {
      iRangeSearchTerm = i;
      iXSearchColumn = constraint.iColumn;
    } 
  }
  if(iSearchTerm >=0) {
    pIdxInfo->idxNum = iXSearchColumn - VSS_INDEX_COLUMN_VECTORS;
    pIdxInfo->idxStr = (char*) "search";
    pIdxInfo->aConstraintUsage[iSearchTerm].argvIndex = 1;
    pIdxInfo->aConstraintUsage[iSearchTerm].omit = 1;
    pIdxInfo->estimatedCost = 300.0;
    pIdxInfo->estimatedRows = 10;
    return SQLITE_OK;
  }
  if(iRangeSearchTerm >=0) {
    pIdxInfo->idxNum = iXSearchColumn - VSS_INDEX_COLUMN_VECTORS;
    pIdxInfo->idxStr = (char*) "range_search";
    pIdxInfo->aConstraintUsage[iRangeSearchTerm].argvIndex = 1;
    pIdxInfo->aConstraintUsage[iRangeSearchTerm].omit = 1;
    pIdxInfo->estimatedCost = 300.0;
    pIdxInfo->estimatedRows = 10;
    return SQLITE_OK;
  }
  pIdxInfo->idxNum = -1;
  pIdxInfo->idxStr = (char*)"fullscan";
  pIdxInfo->estimatedCost = 3000000.0;
  pIdxInfo->estimatedRows = 100000;
  return SQLITE_OK;
}

static int vssIndexFilter(
  sqlite3_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, sqlite3_value **argv
){
  vss_index_cursor *pCur = (vss_index_cursor *)pVtabCursor;  
  if (strcmp(idxStr, "search")==0) {
    pCur->query_type = QueryType::search;
    VssSearchParams* params = (VssSearchParams*) sqlite3_value_pointer(argv[0], "vss0_searchparams");
    int nq = 1;
    pCur->dis  = new std::vector<float>(params->k * nq);
    pCur->nns  = new std::vector<faiss::idx_t>(params->k * nq);
    faiss::Index* index = pCur->table->indexes->at(idxNum);
    index->search(nq, params->vector->data(), params->k, pCur->dis->data(), pCur->nns->data());
    pCur->k = params->k;
    
  }
  else if (strcmp(idxStr, "range_search")==0) {
  pCur->query_type = QueryType::range_search;
    VssRangeSearchParams* params = (VssRangeSearchParams*) sqlite3_value_pointer(argv[0], "vss0_rangesearchparams");
    int nq = 1;
    std::vector<faiss::idx_t> nns(params->distance * nq);
    faiss::RangeSearchResult * result = new faiss::RangeSearchResult(nq, true);
    //pCur->k = params->k;
    faiss::Index* index = pCur->table->indexes->at(idxNum);
    index->range_search(nq, params->vector->data(), params->distance, result);
    pCur->range_search_result = result;
    
  }else if (strcmp(idxStr, "fullscan")==0) {
    pCur->query_type = QueryType::fullscan;
    sqlite3_stmt* stmt;
    int res = sqlite3_prepare_v2(pCur->table->db, sqlite3_mprintf("select rowid from \"%w_data\"", pCur->table->name), -1, &pCur->stmt, NULL);
    if(res != SQLITE_OK) return res;
    pCur->step_result = sqlite3_step(pCur->stmt);
  }
  else {
    if(pVtabCursor->pVtab->zErrMsg != 0) sqlite3_free(pVtabCursor->pVtab->zErrMsg);
    pVtabCursor->pVtab->zErrMsg = sqlite3_mprintf("%s %s", "vssIndexFilter error: unhandled idxStr", idxStr);
    return SQLITE_ERROR;
  }

  pCur->iCurrent = 0;
  return SQLITE_OK;
}

static int vssIndexNext(sqlite3_vtab_cursor *cur){
  vss_index_cursor *pCur = (vss_index_cursor*)cur;
  switch(pCur->query_type) {
    case QueryType::search:
    case QueryType::range_search: {
      pCur->iCurrent++;
      break;
    }
    case QueryType::fullscan: {
      pCur->step_result = sqlite3_step(pCur->stmt);
    }
  }
  
  return SQLITE_OK;
}
static int vssIndexRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid){
  vss_index_cursor *pCur = (vss_index_cursor*)cur;
  switch(pCur->query_type) {
    case QueryType::search: {
      *pRowid = pCur->nns->at(pCur->iCurrent);
      break;
    }
    case QueryType::range_search: {
      *pRowid = pCur->range_search_result->labels[pCur->iCurrent];
      break;
    }
    case QueryType::fullscan: {
      *pRowid = sqlite3_column_int64(pCur->stmt, 0);
      break;
    }
  }
  return SQLITE_OK;
}

static int vssIndexEof(sqlite3_vtab_cursor *cur){
  vss_index_cursor *pCur = (vss_index_cursor*)cur;
  switch(pCur->query_type) {
    case QueryType::search: {
      return pCur->iCurrent >= pCur->k || (pCur->nns->at(pCur->iCurrent) == -1);
    }
    case QueryType::range_search: {      
      return pCur->iCurrent >= pCur->range_search_result->lims[1]; 
    }
    case QueryType::fullscan: {
      return pCur->step_result != SQLITE_ROW;
    }
  }
  return 1;
}

static int vssIndexColumn(
  sqlite3_vtab_cursor *cur,
  sqlite3_context *ctx,
  int i
){
  vss_index_cursor *pCur = (vss_index_cursor*)cur;
  if(i == VSS_INDEX_COLUMN_DISTANCE) {
    switch(pCur->query_type) {
      case QueryType::search: {
        sqlite3_result_double(ctx, pCur->dis->at(pCur->iCurrent));
        break;
      }
      case QueryType::range_search: {
        sqlite3_result_double(ctx, pCur->range_search_result->distances[pCur->iCurrent]);
        break;
      }
      case QueryType::fullscan: {
        break;
      }
    }
  }
  else if ( i >= VSS_INDEX_COLUMN_VECTORS) {
    try {
      faiss::Index * index = pCur->table->indexes->at(i-VSS_INDEX_COLUMN_VECTORS);
      
      std::vector<float> v(index->d);
      sqlite3_int64 rowid;
      vssIndexRowid(cur, &rowid);
      index->reconstruct(rowid, v.data());
      
      json j = json(v);
      sqlite3_result_text(ctx, j.dump().c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_result_subtype(ctx, JSON_SUBTYPE);
    }
    catch(const std::exception& ex) {
      printf("%s\n", ex.what());
      sqlite3_result_null(ctx);
    }
  }
  return SQLITE_OK;
}


static int vssIndexBegin(sqlite3_vtab *tab) {
  //printf("BEGIN\n");
  return SQLITE_OK;
}

static int vssIndexSync(sqlite3_vtab *tab) {
  //printf("SYNC\n");
  return SQLITE_OK;
}

static int vssIndexCommit(sqlite3_vtab *pVTab) {
  vss_index_vtab *p = (vss_index_vtab*)pVTab;
  //printf("COMMIT %d %d\n", p->isTraining, p->isInsertData);
  if(p->isTraining) {
    //printf("TRAINING %lu\n", p->training->size());
    for (std::size_t i = 0; i != p->trainings->size(); ++i) {
      auto training = p->trainings->at(i);
      if(!training.empty()) {
        faiss::Index * index = p->indexes->at(i);
        index->train(training.size() / index->d, training.data());
      }
    }
    p->isTraining = false;
  }
  if(p->isInsertData) {
    //printf("WRITING INDEX\n");
    p->isInsertData = false;

    for(int i = 0; i < p->indexCount; i++) {
      int rc = write_index_insert(p->indexes->at(i), p->db, p->name, i);
      if(rc != SQLITE_OK) {
        return rc;
      }
    }
  }
  return SQLITE_OK;
}

static int vssIndexRollback(sqlite3_vtab *tab) {
  //printf("ROLLBACK\n");
  return SQLITE_OK;
}

static int vssIndexUpdate(
  sqlite3_vtab *pVTab,
  int argc,
  sqlite3_value **argv,
  sqlite_int64 *pRowid
) {
  vss_index_vtab *p = (vss_index_vtab*)pVTab;
  if (argc ==1 && sqlite3_value_type(argv[0]) != SQLITE_NULL) {
    printf("xUpdate DELETE \n");
  }
  else if (argc > 1 && sqlite3_value_type(argv[0])== SQLITE_NULL) {
    // if no operation, we adding it to the index
    bool noOperation = sqlite3_value_type(argv[2+VSS_INDEX_COLUMN_OPERATION]) == SQLITE_NULL;
    if (noOperation) {
      std::vector<float>* vec;
      sqlite3_int64 rowid = sqlite3_value_int64(argv[1]);
      bool inserted_rowid = false;
      for(int i = 0; i < p->indexCount; i++) {
        if ( (vec = valueAsVector(argv[2+VSS_INDEX_COLUMN_VECTORS + i])) != NULL ) {
          if(!inserted_rowid) {
            sqlite_int64 retrowid;
            shadow_data_insert(p->db, p->name, &rowid, &retrowid);
            inserted_rowid = true;
          }
          p->indexes->at(i)->add_with_ids(1, vec->data(), &rowid);
          p->isInsertData = true;
          *pRowid = rowid;
        }
      }
      
      
    } else {
      std::string operation ((char *) sqlite3_value_text(argv[2+VSS_INDEX_COLUMN_OPERATION]));
      if(operation.compare("training") == 0) {
        std::vector<float>* vec;
        for(int i = 0; i < p->indexCount; i++) {
          if ( (vec = valueAsVector(argv[2+VSS_INDEX_COLUMN_VECTORS+i])) != NULL ) {
            p->trainings->at(i).reserve(vec->size() + distance(vec->begin(), vec->end()));
            p->trainings->at(i).insert(p->trainings->at(i).end(), vec->begin(),vec->end());
            p->isTraining = true;
          }
        }      
      } 
      else {
        printf("unknown operation\n");
        return SQLITE_ERROR;
      }
    }
  }
  else {
    //printf("xUpdate UNKNOWN \n");
  }
  return SQLITE_OK;
}


static void vssSearchFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  printf("search?\n");
}
static void faissMemoryUsageFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  sqlite3_result_int64(context, faiss::get_mem_usage_kb());
}
static void vssRangeSearchFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  printf("range search?\n");
}

static int vssIndexFindFunction(
  sqlite3_vtab *pVtab,
  int nArg,
  const char *zName,
  void (**pxFunc)(sqlite3_context*,int,sqlite3_value**),
  void **ppArg
) {
  //printf("find function. %d %s %s \n", nArg, zName, (char *) sqlite3_version);
  if( sqlite3_stricmp(zName, "vss_search")==0 ){
    *pxFunc = vssSearchFunc;
    *ppArg = 0;
    return VSS_SEARCH_FUNCTION;
  }
  if( sqlite3_stricmp(zName, "vss_range_search")==0 ){
    *pxFunc = vssRangeSearchFunc;
    *ppArg = 0;
    return VSS_RANGE_SEARCH_FUNCTION;
  }
  return 0;
};

static int vssIndexShadowName(const char *zName){
  static const char *azName[] = {
    "index",
    "data"
  };
  unsigned int i;
  for(i=0; i<sizeof(azName)/sizeof(azName[0]); i++){
    if( sqlite3_stricmp(zName, azName[i])==0 ) return 1;
  }
  return 0;
}

static sqlite3_module vssIndexModule = {
  /* iVersion    */ 3,
  /* xCreate     */ vssIndexCreate,
  /* xConnect    */ vssIndexConnect,
  /* xBestIndex  */ vssIndexBestIndex,
  /* xDisconnect */ vssIndexDisconnect,
  /* xDestroy    */ vssIndexDestroy,
  /* xOpen       */ vssIndexOpen,
  /* xClose      */ vssIndexClose,
  /* xFilter     */ vssIndexFilter,
  /* xNext       */ vssIndexNext,
  /* xEof        */ vssIndexEof,
  /* xColumn     */ vssIndexColumn,
  /* xRowid      */ vssIndexRowid,
  /* xUpdate     */ vssIndexUpdate,
  /* xBegin      */ vssIndexBegin,
  /* xSync       */ vssIndexSync,
  /* xCommit     */ vssIndexCommit,
  /* xRollback   */ vssIndexRollback,
  /* xFindMethod */ vssIndexFindFunction,
  /* xRename     */ 0,
  /* xSavepoint  */ 0,
  /* xRelease    */ 0,
  /* xRollbackTo */ 0,
  /* xShadowName */ vssIndexShadowName
};

#pragma endregion

#pragma region entrypoint
void cleanup(void *p){}

extern "C" {
  #ifdef _WIN32
  __declspec(dllexport)
  #endif
  int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    SQLITE_EXTENSION_INIT2(pApi);
    sqlite3_create_function_v2(db, "vss_version", 0, SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_INNOCUOUS, 0, vss_version, 0, 0, 0);
    sqlite3_create_function_v2(db, "vss_debug", 0, SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_INNOCUOUS, 0, vss_debug, 0, 0, 0);

    sqlite3_create_function_v2(db, "vss_search", 2, SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_INNOCUOUS, 0, vssSearchFunc, 0, 0, 0);
    sqlite3_create_function_v2(db, "vss_search_params", 2, 0, 0, VssSearchParamsFunc, 0, 0, 0);
    sqlite3_create_function_v2(db, "vss_range_search", 2, SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_INNOCUOUS, 0, vssRangeSearchFunc, 0, 0, 0);
    sqlite3_create_function_v2(db, "vss_range_search_params", 2, 0, 0, VssRangeSearchParamsFunc, 0, 0, 0);
    
    
    sqlite3_create_module_v2  (db, "vss_index", &vssIndexModule, 0, 0);
    return 0;
  }
}

#pragma endregion