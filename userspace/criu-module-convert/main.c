#include "snapshot_reader.h"
#include <stdio.h>

static void usage(const char *p){fprintf(stderr,"usage: %s snapshot.bin [-D output-dir]\n",p);}
int main(int argc,char **argv){const char *input=NULL;struct snapshot_document doc;int i,rc;
	for(i=1;i<argc;i++){if(argv[i][0]=='-'&&argv[i][1]=='D'){if(argv[i][2]){}else if(++i>=argc){usage(argv[0]);return SNAPSHOT_READER_FORMAT_ERROR;}}else if(!input)input=argv[i];else{usage(argv[0]);return SNAPSHOT_READER_FORMAT_ERROR;}}
	if(!input){usage(argv[0]);return SNAPSHOT_READER_FORMAT_ERROR;} rc=snapshot_read_validate(input,&doc);if(rc==SNAPSHOT_READER_OK){snapshot_document_free(&doc);return 0;}snapshot_document_free(&doc);return rc;}
