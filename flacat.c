
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "flacat.h"


/******************************************************************************/

static unsigned int __get_bits(FLAC_DECODER *flacd, int bits, int sign)
{
	int r = 0;
	int s = 0;
	int B = flacd->bit_pos>>3;
	int b = 8-(flacd->bit_pos&7);

	flacd->bit_pos += bits;

	while(s<bits){
		s += b;
		if(s>32){
			r |= flacd->stream_buf[B++] >> (s-32);
			s = 32;
		}else{
			r |= flacd->stream_buf[B++] << (32-s);
		}
		b = 8;
	}

	if(sign){
		r = r >> (32-bits);
	}else{
		r = ((unsigned int)r) >> (32-bits);
	}
	return r;
}


#define get_uint(b)  __get_bits(flacd, b, 0)
#define get_sint(b)  __get_bits(flacd, b, 1)


static int get_rice(FLAC_DECODER *flacd, int param)
{
	unsigned int r = 0;
	while(get_uint(1)==0) r++;
	r = (r<<param) | get_uint(param);
	r = (r>>1) ^ (-(r&1));
	return r;
}


/******************************************************************************/


static void decode_residuals(FLAC_DECODER *flacd, int order, int *obuf)
{
	int method = get_uint(2);
	int param_bits = (method)? 5: 4;
	int escape = (method)? 0x1f: 0x0f;

	int parts = 1<<get_uint(4);
	int psize = flacd->block_size/parts;

	int i, j;
	for(i=0; i<parts; i++){
		int start = i*psize+((i==0)?order:0);
		int end = (i+1)*psize;
		int param = get_uint(param_bits);
		if(param<escape){
			for(j=start; j<end; j++){
				obuf[j] = get_rice(flacd, param);
			}
		}else{
			int nbits = get_uint(5);
			for(j=start; j<end; j++){
				obuf[j] = get_sint(nbits);
			}
		}
	}
}


static void restore_lpc(FLAC_DECODER *flacd, int *coefs, int order, int shift, int *obuf)
{
	int i, j;

	for(i=order; i<flacd->block_size; i++){
		long long sum = 0;
		for(j=0; j<order; j++){
			sum += (long long)obuf[i-1-j]*coefs[j];
		}
		obuf[i] += sum >> shift;
	}

}

/******************************************************************************/


static int fixed_coef[5][4] = {
	{0,},
	{1,},
	{2, -1},
	{3, -3, 1},
	{4, -6, 4, -1},
};


static void decode_lpc(FLAC_DECODER *flacd, int sample_bits, int order, int fixed, int *obuf)
{
	int i, precision, shift;
	int var_coefs[32];
	int *coefs;

	for(i=0; i<order; i++){
		obuf[i] = get_sint(sample_bits);
	}

	if(fixed){
		coefs = fixed_coef[order];
		shift = 0;
	}else{
		precision = get_uint(4)+1;
		shift = get_sint(5);
		for(i=0; i<order; i++){
			var_coefs[i] = get_sint(precision);
		}
		coefs = var_coefs;
	}

	decode_residuals(flacd, order, obuf);
	restore_lpc(flacd, coefs, order, shift, obuf);
}


/******************************************************************************/


int decode_subframe(FLAC_DECODER *flacd, int sample_bits, int *obuf)
{
	get_uint(1);

	int type = get_uint(6);
	int shift = get_uint(1);
	if(shift){
		while(get_uint(1)==0){
			shift++;
		}
	}
	sample_bits -= shift;

	if(type==0){
		// Constant coding
		int const_data = get_sint(sample_bits);
		for(int i=0; i<flacd->block_size; i++){
			obuf[i] = const_data;
		}
	}else if(type==1){
		// Verbatim coding
		for(int i=0; i<flacd->block_size; i++){
			obuf[i] = get_sint(sample_bits);
		}
	}else if(type>=8 && type<=12){
		// Fixed LPC coding
		decode_lpc(flacd, sample_bits, type-8, 1, obuf);
	}else{
		decode_lpc(flacd, sample_bits, type-31, 0, obuf);
	}

	for(int i=0; i<flacd->block_size; i++){
		obuf[i] <<= shift;
	}

	return 0;
}


static char depth_table[8] = {
	0, 8, 12, 0, 16, 20, 24, 32
};


int decode_frame(FLAC_DECODER *flacd)
{
	int i, sample_bits;

	printf("Frame @ %08x\n", flacd->bit_pos>>3);
	if(flacd->bit_pos >= flacd->stream_size)
		return 0;

	int sync = get_uint(16);
	if(sync!=0xfff8){
		return 0;
	}

	int bsize_code = get_uint(4);
	/* sample_rate */get_uint(4);
	int chmode     = get_uint(4);
	int samp_code  = get_uint(3);
	get_uint(1);

	int seq = get_uint(8);
	if(seq>=0x80){
		int n = __builtin_clz(~(seq<<24)) -1;
		flacd->bit_pos += n*8;
	}

	if(bsize_code<2){
		flacd->block_size = 192;
	}else if(bsize_code<6){
		flacd->block_size = 576<<(bsize_code-2);
	}else if(bsize_code==6){
		flacd->block_size = get_uint(8)+1;
	}else if(bsize_code==7){
		flacd->block_size = get_uint(16)+1;
	}else{
		flacd->block_size = 256<<(bsize_code-8);
	}

	sample_bits = flacd->sample_bits;
	if(depth_table[samp_code]){
		sample_bits = depth_table[samp_code];
	}

	get_uint(8); // CRC-8

	if(chmode<8){
		for(i=0; i<(chmode+1); i++){
			decode_subframe(flacd, sample_bits, flacd->out_buf + i*flacd->block_size);
		}
	}else{
		int *out_buf0 = flacd->out_buf;
		int *out_buf1 = out_buf0 + flacd->block_size;
		decode_subframe(flacd, sample_bits+((chmode==9)?1:0), out_buf0);
		decode_subframe(flacd, sample_bits+((chmode==9)?0:1), out_buf1);

		if(chmode==8){
			for(i=0; i<flacd->block_size; i++){
				out_buf1[i] = out_buf0[i] - out_buf1[i];
			}
		}else if(chmode==9){
			for(i=0; i<flacd->block_size; i++){
				out_buf0[i] += out_buf1[i];
			}
		}else{
			for(i=0; i<flacd->block_size; i++){
				int side  = out_buf1[i];
				int right = out_buf0[i] - (side>>1);
				out_buf1[i] = right;
				out_buf0[i] = right + side;
			}
		}
	}

	flacd->bit_pos = (flacd->bit_pos+7)&~7;
	get_uint(16); // crc16

	return 1;
}


/******************************************************************************/

int flac_parse(FLAC_DECODER *flacd, unsigned char *ibuf, int isize)
{
	flacd->stream_buf = ibuf;
	flacd->stream_size = isize*8;
	flacd->bit_pos = 0;

	if(get_uint(32)!=0x664c6143)
		return -1;

	int finish = 0;
	while(finish==0){
		int type = get_uint(8);
		int size = get_uint(24);
		int next_pos = flacd->bit_pos+size*8;

		finish = type&0x80;
		type &= 0x7f;
		printf("META type:%02x\n", type);

		// STREAMINFO
		if(type==0){
			get_uint(16);
			flacd->max_block_size = get_uint(16);
			get_uint(24);
			get_uint(24);
			flacd->sample_rate = get_uint(20);
			flacd->channels    = get_uint(3) + 1;
			flacd->sample_bits = get_uint(5) + 1;
			get_uint(4);
			get_uint(32);


		}

		flacd->bit_pos = next_pos;
	}

	return 0;
}

/******************************************************************************/

