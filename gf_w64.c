/*
 * gf_w64.c
 *
 * Routines for 64-bit Galois fields
 */

#include "gf_int.h"
#include <stdio.h>
#include <stdlib.h>

#define GF_FIELD_WIDTH (64)
#define GF_FIRST_BIT (1L << 63)

#define GF_BASE_FIELD_WIDTH (32)
#define GF_BASE_FIELD_SIZE       (1L << GF_BASE_FIELD_WIDTH)
#define GF_BASE_FIELD_GROUP_SIZE  GF_BASE_FIELD_SIZE-1

// 10000587 is a valid s for 2^16^2
#define GF_S_GF_16_2_2 (1000587)

// 1000012 is a valid s for 2^32
#define GF_S_GF_32_2 (1000012)

struct gf_w64_group_data {
    uint64_t *reduce;
    uint64_t *shift;
    uint64_t *memory;
};

struct gf_split_4_64_lazy_data {
    uint64_t      tables[16][16];
    uint64_t      last_value;
};

struct gf_split_8_64_lazy_data {
    uint64_t      tables[8][(1<<8)];
    uint64_t      last_value;
};

struct gf_split_16_64_lazy_data {
    uint64_t      tables[4][(1<<16)];
    uint64_t      last_value;
};

struct gf_split_8_8_data {
    uint64_t      tables[15][256][256];
};

typedef struct w64_composite_int_s {
  uint64_t s; // 's' will be different depending on the base field
} w64_composite_int_t;

static
inline
gf_val_64_t gf_w64_inverse_from_divide (gf_t *gf, gf_val_64_t a)
{
  return gf->divide.w64(gf, 1, a);
}

static
inline
gf_val_64_t gf_w64_divide_from_inverse (gf_t *gf, gf_val_64_t a, gf_val_64_t b)
{
  b = gf->inverse.w64(gf, b);
  return gf->multiply.w64(gf, a, b);
}

static
void
gf_w64_multiply_region_from_single(gf_t *gf, void *src, void *dest, gf_val_64_t val, int bytes, int
xor)
{
  int i;
  gf_val_64_t *s64;
  gf_val_64_t *d64;

  s64 = (gf_val_64_t *) src;
  d64 = (gf_val_64_t *) dest;

  if (xor) {
    for (i = 0; i < bytes/sizeof(gf_val_64_t); i++) {
      d64[i] ^= gf->multiply.w64(gf, val, s64[i]);
    }
  } else {
    for (i = 0; i < bytes/sizeof(gf_val_64_t); i++) {
      d64[i] = gf->multiply.w64(gf, val, s64[i]);
    }
  }
}

static
inline
gf_val_64_t gf_w64_euclid (gf_t *gf, gf_val_64_t b)
{
  gf_val_64_t e_i, e_im1, e_ip1;
  gf_val_64_t d_i, d_im1, d_ip1;
  gf_val_64_t y_i, y_im1, y_ip1;
  gf_val_64_t c_i;
  gf_val_64_t one = 1;

  if (b == 0) return -1;
  e_im1 = ((gf_internal_t *) (gf->scratch))->prim_poly;
  e_i = b;
  d_im1 = 64;
  for (d_i = d_im1-1; ((one << d_i) & e_i) == 0; d_i--) ;
  y_i = 1;
  y_im1 = 0;

  while (e_i != 1) {

    e_ip1 = e_im1;
    d_ip1 = d_im1;
    c_i = 0;

    while (d_ip1 >= d_i) {
      c_i ^= (one << (d_ip1 - d_i));
      e_ip1 ^= (e_i << (d_ip1 - d_i));
      d_ip1--;
      while ((e_ip1 & (one << d_ip1)) == 0) d_ip1--;
    }

    y_ip1 = y_im1 ^ gf->multiply.w64(gf, c_i, y_i);
    y_im1 = y_i;
    y_i = y_ip1;

    e_im1 = e_i;
    d_im1 = d_i;
    e_i = e_ip1;
    d_i = d_ip1;
  }

  return y_i;
}

/* JSP: GF_MULT_SHIFT: The world's dumbest multiplication algorithm.  I only
   include it for completeness.  It does have the feature that it requires no
   extra memory.  
*/

static
inline
gf_val_64_t
gf_w64_shift_multiply (gf_t *gf, gf_val_64_t a64, gf_val_64_t b64)
{
  uint64_t pl, pr, ppl, ppr, i, pp, a, bl, br, one, lbit;
  gf_internal_t *h;

  h = (gf_internal_t *) gf->scratch;
  ppr = h->prim_poly;
  ppl = 1;
  
  a = a64;
  bl = 0;
  br = b64;
  one = 1;
  lbit = (one << 63);

  pl = 0;
  pr = 0;

  for (i = 0; i < GF_FIELD_WIDTH; i++) {
    if (a & (one << i)) {
      pl ^= bl;
      pr ^= br;
    }
    /* printf("P: %016llx %016llx     ", pl, pr); printf("B: %016llx %016llx\n", bl, br); */
    bl <<= 1;
    if (br & lbit) bl ^= 1;
    br <<= 1;
  }

  one = lbit;
  ppl = ((h->prim_poly >> 1) | lbit);
  ppr = lbit;
  while (one != 0) {
    if (pl & one) {
      pl ^= ppl;
      pr ^= ppr;
    }
    one >>= 1;
    ppr >>= 1;
    if (ppl & 1) ppr ^= lbit;
    ppl >>= 1;
  }
  return pr;
}

#ifdef  INTEL_AES
/*
 * ELM: Use the Intel carryless multiply instruction to do very fast 64x64 multiply.
 */
static
inline
gf_val_64_t
gf_w64_clm_multiply (gf_t *gf, gf_val_64_t a64, gf_val_64_t b64)
{
        __m128i         a, b;
        __m128i         result;
        __m128i         prim_poly;
        __m128i         v;
        gf_internal_t * h = gf->scratch;

        a = _mm_set_epi32 (0, 0, (uint32_t)(a64 >> 32ULL), (uint32_t)(a64 & 0xffffffffULL));
        b = _mm_set_epi32 (0, 0, (uint32_t)(b64 >> 32ULL), (uint32_t)(b64 & 0xffffffffULL));
        prim_poly = _mm_set_epi32(0, 0, 0, (uint32_t)(h->prim_poly & 0xffffffffULL));

        /* Do the initial multiply */
        result = _mm_clmulepi64_si128 (a, b, 0);
        /* Mask off the high order 32 bits using subtraction of the polynomial.
         * NOTE: this part requires that the polynomial have at least 32 leading 0 bits.
         */
        v = _mm_srli_si128 (result, 12);
        result = _mm_xor_si128 (result, _mm_clmulepi64_si128 (prim_poly, v, 0));
        v = _mm_srli_si128 (result, 8);
        result = _mm_xor_si128 (result, _mm_clmulepi64_si128 (prim_poly, v, 0));

        return ((gf_val_64_t)_mm_extract_epi64(result, 0));
}
#endif

void
gf_w64_split_4_64_lazy_multiply_region(gf_t *gf, void *src, void *dest, uint64_t val, int bytes, int xor)
{
  gf_internal_t *h;
  struct gf_split_4_64_lazy_data *ld;
  int i, j, k;
  uint64_t pp, v, s, *s64, *d64, *top;
  gf_region_data rd;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  h = (gf_internal_t *) gf->scratch;
  pp = h->prim_poly;

  ld = (struct gf_split_4_64_lazy_data *) h->private;

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 4);
  gf_do_initial_region_alignment(&rd);

  if (ld->last_value != val) {
    v = val;
    for (i = 0; i < 16; i++) {
      ld->tables[i][0] = 0;
      for (j = 1; j < 16; j <<= 1) {
        for (k = 0; k < j; k++) {
          ld->tables[i][k^j] = (v ^ ld->tables[i][k]);
        }
        v = (v & GF_FIRST_BIT) ? ((v << 1) ^ pp) : (v << 1);
      }
    }
  }
  ld->last_value = val;

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  top = (uint64_t *) rd.d_top;

  while (d64 != top) {
    v = (xor) ? *d64 : 0;
    s = *s64;
    i = 0;
    while (s != 0) {
      v ^= ld->tables[i][s&0xf];
      s >>= 4;
      i++;
    }
    *d64 = v;
    d64++;
    s64++;
  }
  gf_do_final_region_alignment(&rd);
}

static
inline
uint64_t
gf_w64_split_8_8_multiply (gf_t *gf, uint64_t a64, uint64_t b64)
{
  uint64_t product, i, j, mask, tb;
  gf_internal_t *h;
  struct gf_split_8_8_data *d8;
 
  h = (gf_internal_t *) gf->scratch;
  d8 = (struct gf_split_8_8_data *) h->private;
  product = 0;
  mask = 0xff;

  for (i = 0; a64 != 0; i++) {
    tb = b64;
    for (j = 0; tb != 0; j++) {
      product ^= d8->tables[i+j][a64&mask][tb&mask];
      tb >>= 8;
    }
    a64 >>= 8;
  }
  return product;
}

void
gf_w64_split_8_64_lazy_multiply_region(gf_t *gf, void *src, void *dest, uint64_t val, int bytes, int xor)
{
  gf_internal_t *h;
  struct gf_split_8_64_lazy_data *ld;
  int i, j, k;
  uint64_t pp, v, s, *s64, *d64, *top;
  gf_region_data rd;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  h = (gf_internal_t *) gf->scratch;
  pp = h->prim_poly;

  ld = (struct gf_split_8_64_lazy_data *) h->private;

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 4);
  gf_do_initial_region_alignment(&rd);

  if (ld->last_value != val) {
    v = val;
    for (i = 0; i < 8; i++) {
      ld->tables[i][0] = 0;
      for (j = 1; j < 256; j <<= 1) {
        for (k = 0; k < j; k++) {
          ld->tables[i][k^j] = (v ^ ld->tables[i][k]);
        }
        v = (v & GF_FIRST_BIT) ? ((v << 1) ^ pp) : (v << 1);
      }
    }
  }
  ld->last_value = val;

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  top = (uint64_t *) rd.d_top;

  while (d64 != top) {
    v = (xor) ? *d64 : 0;
    s = *s64;
    i = 0;
    while (s != 0) {
      v ^= ld->tables[i][s&0xff];
      s >>= 8;
      i++;
    }
    *d64 = v;
    d64++;
    s64++;
  }
  gf_do_final_region_alignment(&rd);
}

void
gf_w64_split_16_64_lazy_multiply_region(gf_t *gf, void *src, void *dest, uint64_t val, int bytes, int xor)
{
  gf_internal_t *h;
  struct gf_split_16_64_lazy_data *ld;
  int i, j, k;
  uint64_t pp, v, s, *s64, *d64, *top;
  gf_region_data rd;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  h = (gf_internal_t *) gf->scratch;
  pp = h->prim_poly;

  ld = (struct gf_split_16_64_lazy_data *) h->private;

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 4);
  gf_do_initial_region_alignment(&rd);

  if (ld->last_value != val) {
    v = val;
    for (i = 0; i < 4; i++) {
      ld->tables[i][0] = 0;
      for (j = 1; j < (1<<16); j <<= 1) {
        for (k = 0; k < j; k++) {
          ld->tables[i][k^j] = (v ^ ld->tables[i][k]);
        }
        v = (v & GF_FIRST_BIT) ? ((v << 1) ^ pp) : (v << 1);
      }
    }
  }
  ld->last_value = val;

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  top = (uint64_t *) rd.d_top;

  while (d64 != top) {
    v = (xor) ? *d64 : 0;
    s = *s64;
    i = 0;
    while (s != 0) {
      v ^= ld->tables[i][s&0xffff];
      s >>= 16;
      i++;
    }
    *d64 = v;
    d64++;
    s64++;
  }
  gf_do_final_region_alignment(&rd);
}

static 
int gf_w64_shift_init(gf_t *gf)
{
  gf->multiply.w64 = gf_w64_shift_multiply;
  gf->inverse.w64 = gf_w64_euclid;
  gf->multiply_region.w64 = gf_w64_multiply_region_from_single;
  return 1;
}

static
void
gf_w64_group_set_shift_tables(uint64_t *shift, uint64_t val, gf_internal_t *h)
{
  int i;
  uint64_t j;
  uint64_t one = 1;
  int g_s;

  if (h->mult_type == GF_MULT_DEFAULT) {
    g_s = 4;
  } else {
    g_s = h->arg1;
  }
  shift[0] = 0;
 
  for (i = 1; i < (1 << g_s); i <<= 1) {
    for (j = 0; j < i; j++) shift[i|j] = shift[j]^val;
    if (val & (one << 63)) {
      val <<= 1;
      val ^= h->prim_poly;
    } else {
      val <<= 1;
    }
  }
}

static
inline
gf_val_64_t
gf_w64_group_multiply(gf_t *gf, gf_val_64_t a, gf_val_64_t b)
{
  int i;
  uint64_t top, bot, mask, tp;
  int g_s, g_r, lshift, rshift;
  struct gf_w64_group_data *gd;

  gf_internal_t *h = (gf_internal_t *) gf->scratch;
  if (h->mult_type == GF_MULT_DEFAULT) {
    g_s = 4;
    g_r = 8;
  } else {
    g_s = h->arg1;
    g_r = h->arg2;
  }
  gd = (struct gf_w64_group_data *) h->private;
  gf_w64_group_set_shift_tables(gd->shift, b, h);

  mask = ((1 << g_s) - 1);
  top = 0;
  bot = gd->shift[a&mask];
  a >>= g_s; 

  if (a == 0) return bot;
  lshift = 0;
  rshift = 64;

  do {              /* Shifting out is straightfoward */
    lshift += g_s;
    rshift -= g_s;
    tp = gd->shift[a&mask];
    top ^= (tp >> rshift);
    bot ^= (tp << lshift);
    a >>= g_s; 
  } while (a != 0);

  /* Reducing is a bit gross, because I don't zero out the index bits of top.
     The reason is that we throw top away.  Even better, that last (tp >> rshift)
     is going to be ignored, so it doesn't matter how (tp >> 64) is implemented. */
     
  lshift = ((lshift-1) / g_r) * g_r;
  rshift = 64 - lshift;
  mask = (1 << g_r) - 1;
  while (lshift >= 0) {
    tp = gd->reduce[(top >> lshift) & mask];
    top ^= (tp >> rshift);
    bot ^= (tp << lshift);
    lshift -= g_r;
    rshift += g_r;
  }
    
  return bot;
}

static
void gf_w64_group_multiply_region(gf_t *gf, void *src, void *dest, gf_val_64_t val, int bytes, int xor)
{
  int i, fzb;
  uint64_t a64, smask, rmask, top, bot, tp, one;
  int lshift, rshift, g_s, g_r;
  gf_region_data rd;
  uint64_t *s64, *d64, *dtop;
  struct gf_w64_group_data *gd;
  gf_internal_t *h = (gf_internal_t *) gf->scratch;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gd = (struct gf_w64_group_data *) h->private;
  if (h->mult_type == GF_MULT_DEFAULT) {
    g_s = 4;
    g_r = 8;
  } else {
    g_s = h->arg1;
    g_r = h->arg2;
  }
  gf_w64_group_set_shift_tables(gd->shift, val, h);

  for (i = 63; !(val & (1L << i)); i--) ;
  i += g_s;
  if (i > 64) i = 64;   /* i is the bit position of the first zero bit in any element of
                           gd->shift[] */
  fzb = i;

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 4);
  
  gf_do_initial_region_alignment(&rd);

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  dtop = (uint64_t *) rd.d_top;

  smask = (1 << g_s) - 1;
  rmask = (1 << g_r) - 1;

  while (d64 < dtop) {
    a64 = *s64;
    
    top = 0;
    bot = gd->shift[a64&smask];
    a64 >>= g_s;
    i = fzb;

    if (a64 != 0) {
      lshift = 0;
      rshift = 64;
  
      do {  
        lshift += g_s;
        rshift -= g_s;
        tp = gd->shift[a64&smask];
        top ^= (tp >> rshift);
        bot ^= (tp << lshift);
        a64 >>= g_s;
      } while (a64 != 0);
      i += lshift;
  
      lshift = ((i-64-1) / g_r) * g_r;
      rshift = 64 - lshift;
      while (lshift >= 0) {
        tp = gd->reduce[(top >> lshift) & rmask];
        top ^= (tp >> rshift);    
        bot ^= (tp << lshift);
        lshift -= g_r;
        rshift += g_r;
      }
    }

    if (xor) bot ^= *d64;
    *d64 = bot;
    d64++;
    s64++;
  }
  gf_do_final_region_alignment(&rd);
}

static
inline
gf_val_64_t
gf_w64_group_s_equals_r_multiply(gf_t *gf, gf_val_64_t a, gf_val_64_t b)
{
  int i;
  int leftover, rs;
  uint64_t p, l, ind, r, a64;
  int bits_left;
  int g_s;

  struct gf_w64_group_data *gd;
  gf_internal_t *h = (gf_internal_t *) gf->scratch;
  g_s = h->arg1;

  gd = (struct gf_w64_group_data *) h->private;
  gf_w64_group_set_shift_tables(gd->shift, b, h);

  leftover = 64 % g_s;
  if (leftover == 0) leftover = g_s;

  rs = 64 - leftover;
  a64 = a;
  ind = a64 >> rs;
  a64 <<= leftover;
  p = gd->shift[ind];

  bits_left = rs;
  rs = 64 - g_s;

  while (bits_left > 0) {
    bits_left -= g_s;
    ind = a64 >> rs;
    a64 <<= g_s;
    l = p >> rs;
    p = (gd->shift[ind] ^ gd->reduce[l] ^ (p << g_s));
  }
  return p;
}

static
void gf_w64_group_s_equals_r_multiply_region(gf_t *gf, void *src, void *dest, gf_val_64_t val, int bytes, int xor)
{
  int i;
  int leftover, rs;
  uint64_t p, l, ind, r, a64;
  int bits_left;
  int g_s;
  gf_region_data rd;
  uint64_t *s64, *d64, *top;
  struct gf_w64_group_data *gd;
  gf_internal_t *h = (gf_internal_t *) gf->scratch;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gd = (struct gf_w64_group_data *) h->private;
  g_s = h->arg1;
  gf_w64_group_set_shift_tables(gd->shift, val, h);

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 4);
  gf_do_initial_region_alignment(&rd);

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  top = (uint64_t *) rd.d_top;

  leftover = 64 % g_s;
  if (leftover == 0) leftover = g_s;

  while (d64 < top) {
    rs = 64 - leftover;
    a64 = *s64;
    ind = a64 >> rs;
    a64 <<= leftover;
    p = gd->shift[ind];

    bits_left = rs;
    rs = 64 - g_s;

    while (bits_left > 0) {
      bits_left -= g_s;
      ind = a64 >> rs;
      a64 <<= g_s;
      l = p >> rs;
      p = (gd->shift[ind] ^ gd->reduce[l] ^ (p << g_s));
    }
    if (xor) p ^= *d64;
    *d64 = p;
    d64++;
    s64++;
  }
  gf_do_final_region_alignment(&rd);
}


static
int gf_w64_group_init(gf_t *gf)
{
  uint64_t i, j, p, index;
  struct gf_w64_group_data *gd;
  gf_internal_t *h = (gf_internal_t *) gf->scratch;
  int g_r, g_s;

  if (h->mult_type == GF_MULT_DEFAULT) {
    g_s = 4;
    g_r = 8;
  } else {
    g_s = h->arg1;
    g_r = h->arg2;
  }

  gd = (struct gf_w64_group_data *) h->private;
  gd->shift = (uint64_t *) (&(gd->memory));
  gd->reduce = gd->shift + (1 << g_s);

  gd->reduce[0] = 0;
  for (i = 0; i < (1 << g_r); i++) {
    p = 0;
    index = 0;
    for (j = 0; j < g_r; j++) {
      if (i & (1 << j)) {
        p ^= (h->prim_poly << j);
        index ^= (1 << j);
        if (j > 0) index ^= (h->prim_poly >> (64-j)); 
      }
    }
    gd->reduce[index] = p;
  }

  if (g_s == g_r) {
    gf->multiply.w64 = gf_w64_group_s_equals_r_multiply;
    gf->multiply_region.w64 = gf_w64_group_s_equals_r_multiply_region; 
  } else {
    gf->multiply.w64 = gf_w64_group_multiply;
    gf->multiply_region.w64 = gf_w64_group_multiply_region; 
  }
  gf->divide.w64 = NULL;
  gf->inverse.w64 = gf_w64_euclid;

  return 1;
}

static
gf_val_64_t gf_w64_extract_word(gf_t *gf, void *start, int bytes, int index)
{
  uint64_t *r64, rv;

  r64 = (uint64_t *) start;
  rv = r64[index];
  return rv;
}

static
gf_val_64_t gf_w64_composite_extract_word(gf_t *gf, void *start, int bytes, int index)
{
  int sub_size;
  gf_internal_t *h;
  uint8_t *r8, *top;
  uint64_t a, b, *r64;
  gf_region_data rd;

  h = (gf_internal_t *) gf->scratch;
  gf_set_region_data(&rd, gf, start, start, bytes, 0, 0, 32);
  r64 = (uint64_t *) start;
  if (r64 + index < (uint64_t *) rd.d_start) return r64[index];
  if (r64 + index >= (uint64_t *) rd.d_top) return r64[index];
  index -= (((uint64_t *) rd.d_start) - r64);
  r8 = (uint8_t *) rd.d_start;
  top = (uint8_t *) rd.d_top;
  sub_size = (top-r8)/2;

  a = h->base_gf->extract_word.w32(h->base_gf, r8, sub_size, index);
  b = h->base_gf->extract_word.w32(h->base_gf, r8+sub_size, sub_size, index);
  return (a | ((uint64_t)b << 32));
}

static
gf_val_64_t gf_w64_split_extract_word(gf_t *gf, void *start, int bytes, int index)
{
  int i;
  uint64_t *r64, rv;
  uint8_t *r8;
  gf_region_data rd;

  gf_set_region_data(&rd, gf, start, start, bytes, 0, 0, 128);
  r64 = (uint64_t *) start;
  if (r64 + index < (uint64_t *) rd.d_start) return r64[index];
  if (r64 + index >= (uint64_t *) rd.d_top) return r64[index];
  index -= (((uint64_t *) rd.d_start) - r64);
  r8 = (uint8_t *) rd.d_start;
  r8 += ((index & 0xfffffff0)*8);
  r8 += (index & 0xf);
  r8 += 112;
  rv =0;
  for (i = 0; i < 8; i++) {
    rv <<= 8;
    rv |= *r8;
    r8 -= 16;
  }
  return rv;
}

static
inline
gf_val_64_t
gf_w64_bytwo_b_multiply (gf_t *gf, gf_val_64_t a, gf_val_64_t b)
{
  uint64_t prod, pp, bmask;
  gf_internal_t *h;

  h = (gf_internal_t *) gf->scratch;
  pp = h->prim_poly;

  prod = 0;
  bmask = 0x80000000;
  bmask <<= 32;

  while (1) {
    if (a & 1) prod ^= b;
    a >>= 1;
    if (a == 0) return prod;
    if (b & bmask) {
      b = ((b << 1) ^ pp);
    } else {
      b <<= 1;
    }
  }
}

static
inline
gf_val_64_t
gf_w64_bytwo_p_multiply (gf_t *gf, gf_val_64_t a, gf_val_64_t b)
{
  uint64_t prod, pp, pmask, amask;
  gf_internal_t *h;

  h = (gf_internal_t *) gf->scratch;
  pp = h->prim_poly;

  prod = 0;
  pmask = 0x80000000;
  pmask <<= 32;
  amask = 0x80000000;
  amask <<= 32;

  while (amask != 0) {
    if (prod & pmask) {
      prod = ((prod << 1) ^ pp);
    } else {
      prod <<= 1;
    }
    if (a & amask) prod ^= b;
    amask >>= 1;
  }
  return prod;
}

static
void
gf_w64_bytwo_p_nosse_multiply_region(gf_t *gf, void *src, void *dest, gf_val_64_t val, int bytes, int xor)
{
  uint64_t *s64, *d64, t1, t2, ta, prod, amask, pmask, pp;
  gf_region_data rd;
  gf_internal_t *h;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 8);
  gf_do_initial_region_alignment(&rd);

  h = (gf_internal_t *) gf->scratch;

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  pmask = 0x80000000;
  pmask <<= 32;
  pp = h->prim_poly;

  if (xor) {
    while (s64 < (uint64_t *) rd.s_top) {
      prod = 0;
      amask = pmask;
      ta = *s64;
      while (amask != 0) {
        prod = (prod & pmask) ? ((prod << 1) ^ pp) : (prod << 1);
        if (val & amask) prod ^= ta;
        amask >>= 1;
      }
      *d64 ^= prod;
      d64++;
      s64++;
    }
  } else {
    while (s64 < (uint64_t *) rd.s_top) {
      prod = 0;
      amask = pmask;
      ta = *s64;
      while (amask != 0) {
        prod = (prod & pmask) ? ((prod << 1) ^ pp) : (prod << 1);
        if (val & amask) prod ^= ta;
        amask >>= 1;
      }
      *d64 = prod;
      d64++;
      s64++;
    }
  }
  gf_do_final_region_alignment(&rd);
}

static
void
gf_w64_bytwo_b_nosse_multiply_region(gf_t *gf, void *src, void *dest, gf_val_64_t val, int bytes, int xor)
{
  uint64_t *s64, *d64, t1, t2, ta, tb, prod, amask, bmask, pp;
  gf_region_data rd;
  gf_internal_t *h;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 8);
  gf_do_initial_region_alignment(&rd);

  h = (gf_internal_t *) gf->scratch;

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  bmask = 0x80000000;
  bmask <<= 32;
  pp = h->prim_poly;

  if (xor) {
    while (s64 < (uint64_t *) rd.s_top) {
      prod = 0;
      tb = val;
      ta = *s64;
      while (1) {
        if (tb & 1) prod ^= ta;
        tb >>= 1;
        if (tb == 0) break;
        ta = (ta & bmask) ? ((ta << 1) ^ pp) : (ta << 1);
      }
      *d64 ^= prod;
      d64++;
      s64++;
    }
  } else {
    while (s64 < (uint64_t *) rd.s_top) {
      prod = 0;
      tb = val;
      ta = *s64;
      while (1) {
        if (tb & 1) prod ^= ta;
        tb >>= 1;
        if (tb == 0) break;
        ta = (ta & bmask) ? ((ta << 1) ^ pp) : (ta << 1);
      }
      *d64 = prod;
      d64++;
      s64++;
    }
  }
  gf_do_final_region_alignment(&rd);
}
#define MM_PRINT8(s, r) { uint8_t blah[16], ii; printf("%-12s", s); _mm_storeu_si128((__m128i *)blah, r); for (ii = 0; ii < 16; ii += 1) printf("%s%02x", (ii%4==0) ? "   " : " ", blah[15-ii]); printf("\n"); }

#define SSE_AB2(pp, m1 ,m2, va, t1, t2) {\
          t1 = _mm_and_si128(_mm_slli_epi64(va, 1), m1); \
          t2 = _mm_and_si128(va, m2); \
          t2 = _mm_sub_epi64 (_mm_slli_epi64(t2, 1), _mm_srli_epi64(t2, (GF_FIELD_WIDTH-1))); \
          va = _mm_xor_si128(t1, _mm_and_si128(t2, pp)); }

#define BYTWO_P_ONESTEP {\
      SSE_AB2(pp, m1 ,m2, prod, t1, t2); \
      t1 = _mm_and_si128(v, one); \
      t1 = _mm_sub_epi64(t1, one); \
      t1 = _mm_and_si128(t1, ta); \
      prod = _mm_xor_si128(prod, t1); \
      v = _mm_srli_epi64(v, 1); }


void gf_w64_bytwo_p_sse_multiply_region(gf_t *gf, void *src, void *dest, gf_val_64_t val, int bytes, int xor)
{
#ifdef   INTEL_SSE4
  int i;
  uint8_t *s8, *d8;
  uint64_t vrev, one64;
  uint64_t amask;
  __m128i pp, m1, m2, ta, prod, t1, t2, tp, one, v;
  gf_region_data rd;
  gf_internal_t *h;
  
  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 16);
  gf_do_initial_region_alignment(&rd);

  h = (gf_internal_t *) gf->scratch;
  one64 = 1;
  vrev = 0;
  for (i = 0; i < 64; i++) {
    vrev <<= 1;
    if (!(val & (one64 << i))) vrev |= 1;
  }

  s8 = (uint8_t *) rd.s_start;
  d8 = (uint8_t *) rd.d_start;

  amask = -1;
  amask ^= 1;
  pp = _mm_set1_epi64x(h->prim_poly);
  m1 = _mm_set1_epi64x(amask);
  m2 = _mm_set1_epi64x(one64 << 63);
  one = _mm_set1_epi64x(1);

  while (d8 < (uint8_t *) rd.d_top) {
    prod = _mm_setzero_si128();
    v = _mm_set1_epi64x(vrev);
    ta = _mm_load_si128((__m128i *) s8);
    tp = (!xor) ? _mm_setzero_si128() : _mm_load_si128((__m128i *) d8);
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP; BYTWO_P_ONESTEP;
    _mm_store_si128((__m128i *) d8, _mm_xor_si128(prod, tp));
    d8 += 16;
    s8 += 16;
  }
  gf_do_final_region_alignment(&rd);
#endif
}

static
void
gf_w64_bytwo_b_sse_multiply_region(gf_t *gf, void *src, void *dest, gf_val_64_t val, int bytes, int xor)
{
#ifdef   INTEL_SSE4
  uint64_t itb, amask, one64;
  uint8_t *d8, *s8;
  __m128i pp, m1, m2, t1, t2, va, vb;
  struct gf_w32_bytwo_data *btd;
  gf_region_data rd;
  gf_internal_t *h;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 16);
  gf_do_initial_region_alignment(&rd);

  s8 = (uint8_t *) rd.s_start;
  d8 = (uint8_t *) rd.d_start;
  h = (gf_internal_t *) gf->scratch;

  one64 = 1;
  amask = -1;
  amask ^= 1;
  pp = _mm_set1_epi64x(h->prim_poly);
  m1 = _mm_set1_epi64x(amask);
  m2 = _mm_set1_epi64x(one64 << 63);

  while (d8 < (uint8_t *) rd.d_top) {
    va = _mm_load_si128 ((__m128i *)(s8));
    vb = (!xor) ? _mm_setzero_si128() : _mm_load_si128 ((__m128i *)(d8));
    itb = val;
    while (1) {
      if (itb & 1) vb = _mm_xor_si128(vb, va);
      itb >>= 1;
      if (itb == 0) break;
      SSE_AB2(pp, m1, m2, va, t1, t2);
    }
    _mm_store_si128((__m128i *)d8, vb);
    d8 += 16;
    s8 += 16;
  }

  gf_do_final_region_alignment(&rd);
#endif
}


static
int gf_w64_bytwo_init(gf_t *gf)
{
  gf_internal_t *h;

  h = (gf_internal_t *) gf->scratch;

  if (h->mult_type == GF_MULT_BYTWO_p) {
    gf->multiply.w64 = gf_w64_bytwo_p_multiply;
    if (h->region_type == GF_REGION_SSE) {
      gf->multiply_region.w64 = gf_w64_bytwo_p_sse_multiply_region; 
    } else {
      gf->multiply_region.w64 = gf_w64_bytwo_p_nosse_multiply_region;
    } 
  } else {
    gf->multiply.w64 = gf_w64_bytwo_b_multiply;
      if (h->region_type == GF_REGION_SSE) {
      gf->multiply_region.w64 = gf_w64_bytwo_b_sse_multiply_region; 
    } else {
      gf->multiply_region.w64 = gf_w64_bytwo_b_nosse_multiply_region; 
    } 
  }
  gf->inverse.w64 = gf_w64_euclid;
  return 1;
}


static
gf_val_64_t
gf_w64_composite_multiply(gf_t *gf, gf_val_64_t a, gf_val_64_t b)
{
  gf_internal_t *h = (gf_internal_t *) gf->scratch;
  gf_t *base_gf = h->base_gf;
  uint32_t b0 = b & 0x00000000ffffffff;
  uint32_t b1 = (b & 0xffffffff00000000) >> 32;
  uint32_t a0 = a & 0x00000000ffffffff;
  uint32_t a1 = (a & 0xffffffff00000000) >> 32;
  uint32_t a1b1;
  w64_composite_int_t *comp_int = (w64_composite_int_t*)h->private;

  a1b1 = base_gf->multiply.w32(base_gf, a1, b1);

  return ((uint64_t)(base_gf->multiply.w32(base_gf, a0, b0) ^ a1b1) | 
         ((uint64_t)(base_gf->multiply.w32(base_gf, a1, b0) ^ base_gf->multiply.w32(base_gf, a0, b1) ^ base_gf->multiply.w32(base_gf, a1b1, comp_int->s)) << 32));
}

/*
 * Composite field division trick (explained in 2007 tech report)
 *
 * Compute a / b = a*b^-1, where p(x) = x^2 + sx + 1
 *
 * let c = b^-1
 *
 * c*b = (s*b1c1+b1c0+b0c1)x+(b1c1+b0c0)
 *
 * want (s*b1c1+b1c0+b0c1) = 0 and (b1c1+b0c0) = 1
 *
 * let d = b1c1 and d+1 = b0c0
 *
 * solve s*b1c1+b1c0+b0c1 = 0
 *
 * solution: d = (b1b0^-1)(b1b0^-1+b0b1^-1+s)^-1
 *
 * c0 = (d+1)b0^-1
 * c1 = d*b1^-1
 *
 * a / b = a * c
 */
static
gf_val_64_t
gf_w64_composite_inverse(gf_t *gf, gf_val_64_t a)
{
  gf_internal_t *h = (gf_internal_t *) gf->scratch;
  gf_t *base_gf = h->base_gf;
  uint32_t a0 = a & 0x00000000ffffffff;
  uint32_t a1 = (a & 0xffffffff00000000) >> 32;
  uint32_t c0, c1, d, tmp;
  uint64_t c;
  uint32_t a0inv, a1inv;
  w64_composite_int_t *comp_int = (w64_composite_int_t*)h->private;

  if (a0 == 0) {
    a1inv = base_gf->inverse.w32(base_gf, a1);
    c0 = base_gf->multiply.w32(base_gf, a1inv, comp_int->s);
    c1 = a1inv;
  } else if (a1 == 0) {
    c0 = base_gf->inverse.w32(base_gf, a0);
    c1 = 0;
  } else {
    a1inv = base_gf->inverse.w32(base_gf, a1);
    a0inv = base_gf->inverse.w32(base_gf, a0);

    d = base_gf->multiply.w32(base_gf, a1, a0inv);

    tmp = (base_gf->multiply.w32(base_gf, a1, a0inv) ^ base_gf->multiply.w32(base_gf, a0, a1inv) ^ comp_int->s);
    tmp = base_gf->inverse.w32(base_gf, tmp);

    d = base_gf->multiply.w32(base_gf, d, tmp);

    c0 = base_gf->multiply.w32(base_gf, (d^1), a0inv);
    c1 = base_gf->multiply.w32(base_gf, d, a1inv);
  }

  c = c0 | ((uint64_t)c1 << 32);

  return c;
}

static
gf_val_64_t
gf_w64_composite_divide(gf_t *gf, gf_val_64_t a, gf_val_64_t b)
{
  gf_val_64_t binv;

  binv = gf_w64_composite_inverse(gf, b);

  return gf_w64_composite_multiply(gf, a, binv);
}

static
void
gf_w64_composite_multiply_region(gf_t *gf, void *src, void *dest, gf_val_64_t val, int bytes, int xor)
{
  unsigned long uls, uld;
  gf_internal_t *h = (gf_internal_t *) gf->scratch;
  gf_t *base_gf = h->base_gf;
  int i=0;
  uint32_t b0 = val & 0x00000000ffffffff;
  uint32_t b1 = (val & 0xffffffff00000000) >> 32;
  uint64_t *s64, *d64;
  uint64_t *top;
  uint64_t a0, a1, a1b1;
  int num_syms = bytes / 8;
  int sym_divisible = bytes % 4;
  gf_region_data rd;
  w64_composite_int_t *comp_int = (w64_composite_int_t*)h->private;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 8);

  s64 = rd.s_start;
  d64 = rd.d_start;
  top = rd.d_top;
  
  if (xor) {
    while (d64 < top) {
      a0 = *s64 & 0x00000000ffffffff;
      a1 = (*s64 & 0xffffffff00000000) >> 32;
      a1b1 = base_gf->multiply.w32(base_gf, a1, b1);

      *d64 ^= ((uint64_t)(base_gf->multiply.w32(base_gf, a0, b0) ^ a1b1) |
                ((uint64_t)(base_gf->multiply.w32(base_gf, a1, b0) ^ base_gf->multiply.w32(base_gf, a0, b1) ^ base_gf->multiply.w32(base_gf, a1b1, comp_int->s)) << 32));
      s64++;
      d64++;
    }
  } else {
    while (d64 < top) {
      a0 = *s64 & 0x00000000ffffffff;
      a1 = (*s64 & 0xffffffff00000000) >> 32;
      a1b1 = base_gf->multiply.w32(base_gf, a1, b1);

      *d64 = ((base_gf->multiply.w32(base_gf, a0, b0) ^ a1b1) |
                ((uint64_t)(base_gf->multiply.w32(base_gf, a1, b0) ^ base_gf->multiply.w32(base_gf, a0, b1) ^ base_gf->multiply.w32(base_gf, a1b1, comp_int->s)) << 32));
      s64++;
      d64++;
    }
  }
}

static
void
gf_w64_composite_multiply_region_alt(gf_t *gf, void *src, void *dest, gf_val_64_t val, int bytes, int xor)
{
  gf_internal_t *h = (gf_internal_t *) gf->scratch;
  gf_t *base_gf = h->base_gf;
  gf_val_32_t val0 = val & 0x00000000ffffffff;
  gf_val_32_t val1 = (val & 0xffffffff00000000) >> 32;
  uint8_t *slow, *shigh;
  uint8_t *dlow, *dhigh, *top;
  int sub_reg_size;
  gf_region_data rd;
  w64_composite_int_t *comp_int = (w64_composite_int_t*)h->private;

  if (!xor) {
    memset(dest, 0, bytes);
  }
  
  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 32);
  gf_do_initial_region_alignment(&rd);

  slow = (uint8_t *) rd.s_start;
  dlow = (uint8_t *) rd.d_start;
  top = (uint8_t*) rd.d_top;
  sub_reg_size = (top - dlow)/2;
  shigh = slow + sub_reg_size;
  dhigh = dlow + sub_reg_size;

  base_gf->multiply_region.w32(base_gf, slow, dlow, val0, sub_reg_size, xor);
  base_gf->multiply_region.w32(base_gf, shigh, dlow, val1, sub_reg_size, 1);
  base_gf->multiply_region.w32(base_gf, slow, dhigh, val1, sub_reg_size, xor);
  base_gf->multiply_region.w32(base_gf, shigh, dhigh, val0, sub_reg_size, 1);
  base_gf->multiply_region.w32(base_gf, shigh, dhigh, base_gf->multiply.w32(base_gf, comp_int->s, val1), sub_reg_size, 1);

  gf_do_final_region_alignment(&rd);
}



static
int gf_w64_composite_init(gf_t *gf)
{
  gf_internal_t *h = (gf_internal_t *) gf->scratch;

  if (h->region_type & GF_REGION_ALTMAP) {
    gf->multiply_region.w64 = gf_w64_composite_multiply_region_alt;
  } else {
    gf->multiply_region.w64 = gf_w64_composite_multiply_region;
  }

  if (h->base_gf != NULL) {
    gf_internal_t *base_h = (gf_internal_t *) h->base_gf->scratch;
    w64_composite_int_t *comp_int = (w64_composite_int_t*)h->private;

    if (base_h->mult_type == GF_MULT_COMPOSITE) {
      comp_int->s = GF_S_GF_16_2_2; 
    } else {
      comp_int->s = GF_S_GF_32_2; 
    }
  } 

  gf->multiply.w64 = gf_w64_composite_multiply;
  gf->divide.w64 = gf_w64_composite_divide;
  gf->inverse.w64 = gf_w64_composite_inverse;

  return 1;
}

static
void
gf_w64_split_4_64_lazy_sse_altmap_multiply_region(gf_t *gf, void *src, void *dest, uint64_t val, int bytes, int xor)
{
#ifdef INTEL_SSE4
  gf_internal_t *h;
  int i, m, j, k, tindex;
  uint64_t pp, v, s, *s64, *d64, *top;
  __m128i si, tables[16][8], p[8], v0, mask1;
  struct gf_split_4_64_lazy_data *ld;
  uint8_t btable[16];
  gf_region_data rd;

  if (val == 0) { gf_multby_zero(dest, bytes, xor); return; }
  if (val == 1) { gf_multby_one(src, dest, bytes, xor); return; }

  h = (gf_internal_t *) gf->scratch;
  pp = h->prim_poly;
 
  gf_set_region_data(&rd, gf, src, dest, bytes, val, xor, 128);
  gf_do_initial_region_alignment(&rd);

  s64 = (uint64_t *) rd.s_start;
  d64 = (uint64_t *) rd.d_start;
  top = (uint64_t *) rd.d_top;
 
  ld = (struct gf_split_4_64_lazy_data *) h->private;

  v = val;
  for (i = 0; i < 16; i++) {
    ld->tables[i][0] = 0;
    for (j = 1; j < 16; j <<= 1) {
      for (k = 0; k < j; k++) {
        ld->tables[i][k^j] = (v ^ ld->tables[i][k]);
      }
      v = (v & GF_FIRST_BIT) ? ((v << 1) ^ pp) : (v << 1);
    }
    for (j = 0; j < 8; j++) {
      for (k = 0; k < 16; k++) {
        btable[k] = (uint8_t) ld->tables[i][k];
        ld->tables[i][k] >>= 8;
      }
      tables[i][j] = _mm_loadu_si128((__m128i *) btable);
    }
  }

  mask1 = _mm_set1_epi8(0xf);

  while (d64 != top) {

    if (xor) {
      for (i = 0; i < 8; i++) p[i] = _mm_load_si128 ((__m128i *) (d64+i*2));
    } else {
      for (i = 0; i < 8; i++) p[i] = _mm_setzero_si128();
    }
    i = 0;
    for (k = 0; k < 8; k++) {
      v0 = _mm_load_si128((__m128i *) s64); 
      s64 += 2;
      
      si = _mm_and_si128(v0, mask1);
  
      /* Happy now? */
      for (j = 0; j < 8; j++) {
        p[j] = _mm_xor_si128(p[j], _mm_shuffle_epi8(tables[i][j], si));
      }
      i++;
      v0 = _mm_srli_epi32(v0, 4);
      si = _mm_and_si128(v0, mask1);
      for (j = 0; j < 8; j++) {
        p[j] = _mm_xor_si128(p[j], _mm_shuffle_epi8(tables[i][j], si));
      }
      i++;
    }
    for (i = 0; i < 8; i++) {
      _mm_store_si128((__m128i *) d64, p[i]);
      d64 += 2;
    }
  }
  gf_do_final_region_alignment(&rd);
#endif
}

#define GF_MULTBY_TWO(p) (((p) & GF_FIRST_BIT) ? (((p) << 1) ^ h->prim_poly) : (p) << 1);

static
int gf_w64_split_init(gf_t *gf)
{
  gf_internal_t *h;
  struct gf_split_4_64_lazy_data *d4;
  struct gf_split_8_64_lazy_data *d8;
  struct gf_split_8_8_data *d88;
  struct gf_split_16_64_lazy_data *d16;
  uint64_t p, basep;
  int exp, i, j;

  h = (gf_internal_t *) gf->scratch;

  /* Defaults */
  gf->multiply_region.w64 = gf_w64_multiply_region_from_single;
  gf->multiply.w64 = gf_w64_shift_multiply;
  gf->inverse.w64 = gf_w64_euclid;

  if ((h->arg1 == 4 && h->arg2 == 64) || (h->arg1 == 64 && h->arg2 == 4)) {
    d4 = (struct gf_split_4_64_lazy_data *) h->private;
    d4->last_value = 0;
    if (h->region_type & GF_REGION_SSE) {
      if (h->region_type & GF_REGION_ALTMAP) {
        gf->multiply_region.w64 = gf_w64_split_4_64_lazy_sse_altmap_multiply_region; 
      } else {
/*        gf->multiply_region.w32 = gf_w32_split_4_32_lazy_sse_multiply_region; */
      }
    } else {
      gf->multiply_region.w64 = gf_w64_split_4_64_lazy_multiply_region;
    }
  }
  if ((h->arg1 == 8 && h->arg2 == 64) || (h->arg1 == 64 && h->arg2 == 8)) {
    d8 = (struct gf_split_8_64_lazy_data *) h->private;
    d8->last_value = 0;
    gf->multiply_region.w64 = gf_w64_split_8_64_lazy_multiply_region;
  }
  if ((h->arg1 == 16 && h->arg2 == 64) || (h->arg1 == 64 && h->arg2 == 16)) {
    d16 = (struct gf_split_16_64_lazy_data *) h->private;
    d16->last_value = 0;
    gf->multiply_region.w64 = gf_w64_split_16_64_lazy_multiply_region;
  }
  if ((h->arg1 == 8 && h->arg2 == 8)) {
    d88 = (struct gf_split_8_8_data *) h->private;
    gf->multiply.w64 = gf_w64_split_8_8_multiply;
    /* The performance of this guy sucks, so don't bother with a region op */
    basep = 1;
    for (exp = 0; exp < 15; exp++) {
      for (j = 0; j < 256; j++) d88->tables[exp][0][j] = 0;
      for (i = 0; i < 256; i++) d88->tables[exp][i][0] = 0;
      d88->tables[exp][1][1] = basep;
      for (i = 2; i < 256; i++) {
        if (i&1) {
          p = d88->tables[exp][i^1][1];
          d88->tables[exp][i][1] = p ^ basep;
        } else {
          p = d88->tables[exp][i>>1][1];
          d88->tables[exp][i][1] = GF_MULTBY_TWO(p);
        }
      }
      for (i = 1; i < 256; i++) {
        p = d88->tables[exp][i][1];
        for (j = 1; j < 256; j++) {
          if (j&1) {
            d88->tables[exp][i][j] = d88->tables[exp][i][j^1] ^ p;
          } else {
            d88->tables[exp][i][j] = GF_MULTBY_TWO(d88->tables[exp][i][j>>1]);
          }
        }
      }
      for (i = 0; i < 8; i++) basep = GF_MULTBY_TWO(basep);
    }
  }
  return -1;
}

int gf_w64_scratch_size(int mult_type, int region_type, int divide_type, int arg1, int arg2)
{
  int ss, sa;

  ss = (GF_REGION_SSE | GF_REGION_NOSSE);
  sa = (GF_REGION_STDMAP | GF_REGION_ALTMAP);

  if (divide_type == GF_DIVIDE_MATRIX) return -1;
  switch(mult_type)
  {
    case GF_MULT_SHIFT:
      if (arg1 != 0 || arg2 != 0 || region_type != 0) return -1;
      return sizeof(gf_internal_t);
      break;
    case GF_MULT_BYTWO_p:
    case GF_MULT_BYTWO_b:
      if (arg1 != 0 || arg2 != 0) return -1;
      if (region_type != GF_REGION_CAUCHY) {
        if ((region_type | ss) != ss || (region_type & ss) == ss) return -1;
      }
      return sizeof(gf_internal_t);
      break;

    case GF_MULT_SPLIT_TABLE:
        if (arg1 == 8 && arg2 == 8) {
          region_type &= (~GF_REGION_LAZY);
          if (region_type != GF_REGION_DEFAULT) return -1;
          return sizeof(gf_internal_t) + sizeof(struct gf_split_8_8_data) + 64;
        }
        if ((arg1 == 16 && arg2 == 64) || (arg2 == 16 && arg1 == 64)) {
          region_type &= (~GF_REGION_LAZY);
          if (region_type != GF_REGION_DEFAULT) return -1;
          return sizeof(gf_internal_t) + sizeof(struct gf_split_16_64_lazy_data) + 64;
        }
        if ((arg1 == 8 && arg2 == 64) || (arg2 == 8 && arg1 == 64)) {
          region_type &= (~GF_REGION_LAZY);
          if (region_type != GF_REGION_DEFAULT) return -1;
          return sizeof(gf_internal_t) + sizeof(struct gf_split_8_64_lazy_data) + 64;
        }

        if ((arg1 == 64 && arg2 == 4) || (arg1 == 4 && arg2 == 64)){
          region_type &= (~GF_REGION_LAZY);
          if ((region_type & ss) == ss) return -1;
          if ((region_type & sa) == sa) return -1;
          if (region_type & (~(ss|sa))) return -1;
          if (region_type & GF_REGION_SSE) {
            return sizeof(gf_internal_t) + sizeof(struct gf_split_4_64_lazy_data) + 64;
          } else if (region_type & GF_REGION_ALTMAP) {
            return -1;
          } else {
            return sizeof(gf_internal_t) + sizeof(struct gf_split_4_64_lazy_data) + 64;
          }
        }
        return -1;

    case GF_MULT_DEFAULT:
      arg1 = 4;
      arg2 = 8;
    case GF_MULT_GROUP:
      if (arg1 <= 0 || arg2 <= 0) return -1;
      if (region_type != GF_REGION_DEFAULT && region_type != GF_REGION_CAUCHY) return -1;
      return sizeof(gf_internal_t) + sizeof(struct gf_w64_group_data) +
               sizeof(uint64_t) * (1 << arg1) +
               sizeof(uint64_t) * (1 << arg2) + 64;
      break;
    case GF_MULT_COMPOSITE:
      if (region_type & ~(GF_REGION_ALTMAP | GF_REGION_STDMAP)) return -1;
      if ((arg1 == 2 && arg2 == 0) || (arg1 == 2 && arg2 == 1)) {
        return sizeof(gf_internal_t) + sizeof(w64_composite_int_t) + 4;
      } else {
        return -1;
      }
      break;
    default:
      return -1;
   }
}

int gf_w64_init(gf_t *gf)
{
  gf_internal_t *h;

  h = (gf_internal_t *) gf->scratch;
  if (h->prim_poly == 0) h->prim_poly = 0x1b; /* Omitting the leftmost 1 as in w=32 */

  gf->multiply.w64 = NULL;
  gf->divide.w64 = NULL;
  gf->inverse.w64 = NULL;
  gf->multiply_region.w64 = NULL;

  switch(h->mult_type) {
    case GF_MULT_SHIFT:     if (gf_w64_shift_init(gf) == 0) return 0; break;
    case GF_MULT_COMPOSITE: if (gf_w64_composite_init(gf) == 0) return 0; break;
    case GF_MULT_SPLIT_TABLE: if (gf_w64_split_init(gf) == 0) return 0; break; 
    case GF_MULT_DEFAULT:
    case GF_MULT_GROUP:       if (gf_w64_group_init(gf) == 0) return 0; break; 
    case GF_MULT_BYTWO_p:
    case GF_MULT_BYTWO_b:     if (gf_w64_bytwo_init(gf) == 0) return 0; break;
    default: return 0;
  }
  if (h->divide_type == GF_DIVIDE_EUCLID) {
    gf->divide.w64 = gf_w64_divide_from_inverse;
    gf->inverse.w64 = gf_w64_euclid;
  } 

/* else if (h->divide_type == GF_DIVIDE_MATRIX) {
    gf->divide.w64 = gf_w64_divide_from_inverse;
    gf->inverse.w64 = gf_w64_matrix;
  } */

  if (gf->inverse.w64 != NULL && gf->divide.w64 == NULL) {
    gf->divide.w64 = gf_w64_divide_from_inverse;
  }
  if (gf->inverse.w64 == NULL && gf->divide.w64 != NULL) {
    gf->inverse.w64 = gf_w64_inverse_from_divide;
  }

  if (h->region_type & GF_REGION_ALTMAP) {
    if (h->mult_type == GF_MULT_COMPOSITE) {
      gf->extract_word.w64 = gf_w64_composite_extract_word;
    } else if (h->mult_type == GF_MULT_SPLIT_TABLE) {
      gf->extract_word.w64 = gf_w64_split_extract_word;
    }
  } else {
    gf->extract_word.w64 = gf_w64_extract_word;
  }

  return 1;
}
