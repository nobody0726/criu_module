#include "snapshot_reader.h"
#include "criu_model.h"
#include <stdio.h>

static void usage(const char *p){fprintf(stderr,"usage: %s snapshot.bin [-D output-dir]\n",p);}
int main(int argc,char **argv){const char *input=NULL,*outdir=".";struct snapshot_document doc;int i,rc;
	for(i=1;i<argc;i++){if(argv[i][0]=='-'&&argv[i][1]=='D'){if(argv[i][2])outdir=argv[i]+2;else if(++i>=argc){usage(argv[0]);return SNAPSHOT_READER_FORMAT_ERROR;}else outdir=argv[i];}else if(!input)input=argv[i];else{usage(argv[0]);return SNAPSHOT_READER_FORMAT_ERROR;}}
	if(!input){usage(argv[0]);return SNAPSHOT_READER_FORMAT_ERROR;} rc=snapshot_read_validate(input,&doc);if(rc==SNAPSHOT_READER_OK){struct criu_convert_options o={.output_dir=outdir};rc=criu_emit_images(&doc,&o);}snapshot_document_free(&doc);return rc;}
