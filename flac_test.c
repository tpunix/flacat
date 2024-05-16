
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "flacat.h"


int main(int argc, char *argv[])
{
	FILE *fp;
	int fsize;
	unsigned char *fbuf;

	fp = fopen(argv[1], "rb");
	if(fp==NULL)
		return -1;
	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	fbuf = (unsigned char*)malloc(fsize);
	fread(fbuf, 1, fsize, fp);
	fclose(fp);

	FLAC_DECODER flacd;
	memset(&flacd, 0, sizeof(flacd));

	if(flac_parse(&flacd, fbuf, fsize)<0){
		printf("Not a FLAC file!\n");
		return -1;
	};

	printf("    max_block_size: %d\n", flacd.max_block_size);
	printf("    sample_rate: %d\n", flacd.sample_rate);
	printf("    channels   : %d\n", flacd.channels);
	printf("    sample_bits: %d\n", flacd.sample_bits);

	flacd.out_buf = malloc(flacd.max_block_size * flacd.channels * sizeof(int));

	FILE *wave_fp = fopen(argv[2], "wb+");
	int wave_size = 0;

	// write wave header
	char hdr[64];
	memset(hdr, 0, 64);

	strcpy(hdr+0x00, "RIFF");
	strcpy(hdr+0x08, "WAVE");
	strcpy(hdr+0x0c, "fmt ");
	*(int*)(hdr+0x10) = 0x10;
	*(short*)(hdr+0x14) = 0x0001;
	*(short*)(hdr+0x16) = flacd.channels;
	*(int*)(hdr+0x18) = flacd.sample_rate;
	*(int*)(hdr+0x1c) = flacd.sample_rate * flacd.channels * (flacd.sample_bits/8);
	*(short*)(hdr+0x20) = flacd.channels * (flacd.sample_bits/8);
	*(short*)(hdr+0x22) = flacd.sample_bits;
	strcpy(hdr+0x24, "data");

	fwrite(hdr, 0x2c, 1, wave_fp);

	while(1){
		if(decode_frame(&flacd)==0)
			break;

		int bytes = flacd.sample_bits/8;
		for(int i=0; i<flacd.block_size; i++){
			int *obuf = flacd.out_buf+i;
			for(int j=0; j<flacd.channels; j++){
				fwrite(obuf, bytes, 1, wave_fp);
				obuf += flacd.block_size;
			}
		}

		wave_size += flacd.block_size * flacd.channels * bytes;
	}

	fseek(wave_fp, 0, SEEK_SET);
	*(int*)(hdr+0x04) = wave_size+0x2c-8;
	*(int*)(hdr+0x28) = wave_size;
	fwrite(hdr, 0x2c, 1, wave_fp);

	fclose(wave_fp);

	return 0;
}

