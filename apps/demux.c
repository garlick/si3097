#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "demux.h"

/* demultiplex 4 quadrant camera data */
/*

  data gets emptied like this:

   1-----<-------   2----->-------
   |            |   |            |
   |     ^      |   |     ^      |
   |     |      |   |     |      |
   |            |   |            |
   |------------|   |------------|

   3-------------   4-------------
   |            |   |            |
   |     |      |   |     |      |
   |     v      |   |     v      |
   |            |   |            |
   |-----<------|   |----->------|


Whole image is size by size or 4096x4096

*/

/* try to make a demux that works on old and new configurations */

/* size 4096 serlen 2047 parlen 2046 */

/* size 2048 serlen 1023 parlen 1023 */

void si_camera_demux_gen( unsigned short *out, unsigned short *in, int size,
                          int serlen, int parlen )
{
  int so2, tot, row, col, irow, icol;

  so2 = size/2;
  tot = size*size; /* number of elements in data */

  row = 0;
  col = 0;
  tot = 0;
  for( row = 0; row < parlen; row++ ) {
    for( col = 0; col < serlen; col++ ) {
      out[ (row+2)*size + (col+1) ]  = in[tot];

      icol = (serlen - (col+1)) + so2 ;
      irow = (parlen - (row+1)) + so2;
      out[ (row+2)*size + icol ]  = in[tot+1];
      out[ irow*size + col+1 ]  = in[tot+2];
      out[ irow*size + icol ]  = in[tot+3];

      tot+=4;
    }
  }
}

