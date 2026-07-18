/*                               -*- Mode: C -*- 
 * $Basename: docid.c $
 * $Revision: 1.5.1.8.1.3 $
 * Author          : Brewster Kahle
 * Created On      : 92/02/12  13:17:44
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Mon May  5 11:37:59 1997
 * Language        : C
 * Update Count    : 1
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1997, Universität Dortmund, all rights reserved.
 * (C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)
 */

/*
 * these are the methods for the DocID struct
 * -brewster
 */

#ifndef lint
static char *PRCSid = "$Id: docid.c 1.5.1.8.1.3 Mon, 05 May 1997 11:54:27 +0200 pfeifer $";
#endif

#include "docid.h"
#include "irfileio.h"
#include "cutil.h"
#include "cdialect.h"

#define TESTDOC(doc) if((doc) == NULL) return NULL

/*---------------------------------------------------------------------------*/

/* get the server slot */
any* GetServer(doc)
DocID* doc;
{
  TESTDOC(doc);
  if(NULL != doc->distributorServer)
    return(doc->distributorServer);
  else return(doc->originalServer);
}

/*---------------------------------------------------------------------------*/

any* GetDatabase(doc)
DocID* doc;
{
  TESTDOC(doc);
  if(NULL != doc->distributorDatabase)
    return(doc->distributorDatabase);
  else return(doc->originalDatabase);
}

/*---------------------------------------------------------------------------*/

any* GetLocalID(doc)
DocID* doc;
{
  TESTDOC(doc);
  if(NULL != doc->distributorLocalID)
    return(doc->distributorLocalID);
  else return(doc->originalLocalID);
}

/*---------------------------------------------------------------------------*/

long GetCopyrightDisposition(doc)
DocID* doc;
{
  if(doc)
    return(doc->copyrightDisposition);
  else return 0;
}

/*---------------------------------------------------------------------------*/

/* returns FALSE if it loses, TRUE if it wins */
long ReadDocID(doc,file)
DocID* doc;
FILE* file;
{
  char temp_string[MAX_SYMBOL_SIZE];
  if(FALSE == CheckStartOfStruct("doc-id", file)){
    return(FALSE);
  }
  doc->originalServer = NULL;
  doc->originalDatabase = NULL;
  doc->originalLocalID = NULL;
  doc->distributorServer = NULL;
  doc->distributorDatabase = NULL;
  doc->distributorLocalID = NULL;
  doc->copyrightDisposition = COPY_WITHOUT_RESTRICTION;
	
  while(TRUE){
    long result = ReadSymbol(temp_string, file, 
			     (long)MAX_SYMBOL_SIZE);
		
    if(FALSE == result){ 
      return(FALSE);
    }
    if(END_OF_STRUCT_OR_LIST == result){
      break;
    }
    if(0 == strcmp(temp_string, ":original-server")){
      any* an_any = (any*)s_malloc(sizeof(any));
      ReadAny(an_any, file);
      doc->originalServer = an_any;
    }
    else if(0 == strcmp(temp_string, ":original-database")){
      any* an_any = (any*)s_malloc(sizeof(any));
      ReadAny(an_any, file);
      doc->originalDatabase = an_any;
    }
    else if(0 == strcmp(temp_string, ":original-local-id")){
      any* an_any = (any*)s_malloc(sizeof(any));
      ReadAny(an_any, file);
      doc->originalLocalID = an_any;
    }
    else if(0 == strcmp(temp_string, ":distributor-server")){
      any* an_any = (any*)s_malloc(sizeof(any));
      ReadAny(an_any, file);
      doc->distributorServer = an_any;
    }
    else if(0 == strcmp(temp_string, ":distributor-database")){
      any* an_any = (any*)s_malloc(sizeof(any));
      ReadAny(an_any, file);
      doc->distributorDatabase = an_any;
    }
    else if(0 == strcmp(temp_string, ":distributor-local-id")){
      any* an_any = (any*)s_malloc(sizeof(any));
      ReadAny(an_any, file);
      doc->distributorLocalID = an_any;
    }
    else if(0 == strcmp(temp_string, ":copyright-disposition"))
      ReadLong(file,&doc->copyrightDisposition);
    else{
      SkipObject(file);
    }
  }
  return(TRUE);
}


/*---------------------------------------------------------------------------*/

#ifdef CRYPT_IDS
#define WriteAnySymbol(A,B,C) WriteSymbol(A,C); WriteAny(B,C)
#else
/* this is a local function, only used in WriteDocID() */
static void WriteAnySymbol(str1,Any,file)
char *str1;
any* Any;
FILE* file;
{
char outstr[MAXNAMLEN], str2[MAXNAMLEN];
int i;

#ifdef ASCII_ID
  for(i=0; i<=Any->size; i++) {
    str2[i] = Any->bytes[i];
    }
  if (i<MAXNAMLEN) {
    str2[i] = '\0';
  } else {
    str2[i-1] = '\0';
  }
#ifdef ASCII_ID_TEST
  fprintf(stderr,"WriteAnySymbol():'%s' '%s'\n", str1, str2);
  fflush(stderr);
#endif
  sprintf(outstr,"%s %s ", str1, str2);
#else
  strcpy(outstr, str1);
#endif
  WriteSymbol(outstr, file);
  WriteAny(Any, file);
}
#endif /* CRYPT_IDS */

/* this writes a CDocID to a stream */
long WriteDocID(doc,file)
DocID* doc;
FILE* file;
{
  WriteNewline(file);
  WriteStartOfStruct("doc-id", file);
  if (NULL != doc->originalServer){
    WriteNewline(file);
    WriteAnySymbol(":original-server", doc->originalServer, file);
  }
  if (NULL != doc->originalDatabase){
    WriteNewline(file);
    WriteAnySymbol(":original-database", doc->originalDatabase, file);
  }
  if (NULL != doc->originalLocalID){
    WriteNewline(file);
    WriteAnySymbol(":original-local-id", doc->originalLocalID, file);
  }
  if (NULL != doc->distributorServer){
    WriteNewline(file);
    WriteAnySymbol(":distributor-server", doc->distributorServer, file);
  }
  if (NULL != doc->distributorDatabase){
    WriteNewline(file);
    WriteAnySymbol(":distributor-database", doc->distributorDatabase, file);
  }
  if (NULL != doc->distributorLocalID){
    WriteNewline(file);
    WriteAnySymbol(":distributor-local-id", doc->distributorLocalID, file);
  }
  WriteNewline(file);
  WriteSymbol(":copyright-disposition", file);
  WriteLong(doc->copyrightDisposition, file);
  return(WriteEndOfStruct(file));
}

/*---------------------------------------------------------------------------*/

	static Boolean safeCmp _AP((any* a1,any* a2));

	static Boolean safeCmp(a1,a2)
        any* a1;
        any* a2;
        {
		/* compare 2 any's, either of which may be NULL */
		if (a1 == NULL && a2 == NULL)
		  	return(true);
		else if (a1 == NULL || a2 == NULL)
		  	return(false);
		else if (a1->size != a2->size)
		  	return(false);
		else if (memcmp(a1->bytes,a2->bytes,(size_t)a1->size) == 0)
		  	return(true);
		else
			return(false);
		}

/*---------------------------------------------------------------------------*/

Boolean cmpDocIDs(d1,d2)
DocID* d1;
DocID* d2;
{
  if (safeCmp(d1->originalServer,d2->originalServer) &&
      safeCmp(d1->originalDatabase,d2->originalDatabase) &&
      safeCmp(d1->originalLocalID,d2->originalLocalID) &&
      safeCmp(d1->distributorServer,d2->distributorServer) &&
      safeCmp(d1->distributorDatabase,d2->distributorDatabase) &&
      safeCmp(d1->distributorLocalID,d2->distributorLocalID) &&
      d1->copyrightDisposition == d2->copyrightDisposition)
    return(true);
  else
    return(false); 
}
	
/*---------------------------------------------------------------------------*/

DocID* 
makeDocID()
{
  return((DocID*)s_malloc((size_t)sizeof(DocID)));
}

/*---------------------------------------------------------------------------*/

DocID*
copyDocID(doc)
DocID* doc;
{
  DocID* result = NULL;
  if(doc != NULL && 
     (result = makeDocID()) != NULL) {
    result->originalServer = duplicateAny(doc->originalServer);
    result->originalDatabase = duplicateAny(doc->originalDatabase);
    result->originalLocalID = duplicateAny(doc->originalLocalID);
    result->distributorServer = duplicateAny(doc->distributorServer);
    result->distributorDatabase = duplicateAny(doc->distributorDatabase);
    result->distributorLocalID = duplicateAny(doc->distributorLocalID);
  }
  return result;
}

/*---------------------------------------------------------------------------*/

void freeDocID(doc)
DocID* doc;
{
  if(doc) {
    freeAny(doc->originalServer);
    freeAny(doc->originalDatabase);
    freeAny(doc->originalLocalID);
    freeAny(doc->distributorServer);
    freeAny(doc->distributorDatabase);
    freeAny(doc->distributorLocalID);
    s_free(doc);
  }
}

/*---------------------------------------------------------------------------*/

#define DT_OriginalServer 	(data_tag)1
#define DT_OriginalDatabase 	(data_tag)2
#define DT_OriginalLocalID	(data_tag)3
#define DT_DistributorServer	(data_tag)4
#define DT_DistributorDatabase	(data_tag)5
#define DT_DistributorLocalID	(data_tag)6
#define DT_CopyrightDispostion	(data_tag)7

DocID*
docIDFromAny(rawDocID)
any* rawDocID;
/* read from a z3950 docid to a docid structure */
{
  DocID* docID;
  char* buf;
  
  TESTDOC(rawDocID);

  buf = rawDocID->bytes;
  docID = makeDocID();
  while ((buf - rawDocID->bytes) < rawDocID->size)
    { data_tag tag = peekTag(buf);
      switch (tag)
	{ case DT_OriginalServer:
	    buf = readAny(&(docID->originalServer),buf);
	    break;
	  case DT_OriginalDatabase:
	    buf = readAny(&(docID->originalDatabase),buf);
	    break;
	  case DT_OriginalLocalID:
	    buf = readAny(&(docID->originalLocalID),buf);
	    break;
	  case DT_DistributorServer:
	    buf = readAny(&(docID->distributorServer),buf);
	    break;
	  case DT_DistributorDatabase:
	    buf = readAny(&(docID->distributorDatabase),buf);
	    break;
	  case DT_DistributorLocalID:
	    buf = readAny(&(docID->distributorLocalID),buf);
	    break;
	  case DT_CopyrightDispostion:
	    buf = readNum(&(docID->copyrightDisposition),buf);
	    break;
	  default:
	    freeDocID(docID);
	    return(NULL);
	  };
    }
  
  return(docID);
}

/*---------------------------------------------------------------------------*/

any*
anyFromDocID(docID)
DocID* docID;
/* write a docid structure to a buffer in z3950 format */
{
  any* rawDocID = NULL;
  char* buf = NULL;
  char* data = NULL;
  long size,bytesLeft;
  
  if (docID == NULL)
    return(NULL);
    
  size = writtenAnySize(DT_OriginalServer,docID->originalServer);
  size += writtenAnySize(DT_OriginalDatabase,docID->originalDatabase);
  size += writtenAnySize(DT_OriginalLocalID,docID->originalLocalID);
  size += writtenAnySize(DT_DistributorServer,docID->distributorServer);
  size += writtenAnySize(DT_DistributorDatabase,docID->distributorDatabase);
  size += writtenAnySize(DT_DistributorLocalID,docID->distributorLocalID);
  size += writtenNumSize(DT_CopyrightDispostion,docID->copyrightDisposition);
  
  data = s_malloc((size_t)(sizeof(char) * size));
  
  buf = data;
  bytesLeft = size;
  
  buf = writeAny(docID->originalServer,DT_OriginalServer,buf,&bytesLeft);
  buf = writeAny(docID->originalDatabase,DT_OriginalDatabase,buf,&bytesLeft);
  buf = writeAny(docID->originalLocalID,DT_OriginalLocalID,buf,&bytesLeft);
  buf = writeAny(docID->distributorServer,DT_DistributorServer,buf,
                 &bytesLeft);
  buf = writeAny(docID->distributorDatabase,DT_DistributorDatabase,
                 buf,&bytesLeft);
  buf = writeAny(docID->distributorLocalID,DT_DistributorLocalID,buf,
                 &bytesLeft);
  buf = writeNum(docID->copyrightDisposition,DT_CopyrightDispostion,
                 buf,&bytesLeft);
  
  rawDocID = makeAny(size,data);
  
  return(rawDocID);
}

/*---------------------------------------------------------------------------*/
