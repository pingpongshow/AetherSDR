/* Reed-Solomon decoder
 * Copyright 2002 Phil Karn, KA9Q
 * May be used under the terms of the GNU General Public License (GPL)
 */

#ifdef DEBUG
#include <stdio.h>
#endif

#include <stdlib.h>
#include <string.h>

#define min(a,b) ((a) < (b) ? (a) : (b))

#ifdef FIXED
#include "fixed.h"
#elif defined(BIGSYM)
#include "int.h"
#else
#include "char.h"
#endif

int DECODE_RS(
#ifdef FIXED
DTYPE *data, int *eras_pos, int no_eras, int pad) {
#else
void *p, DTYPE *data, int *eras_pos, int no_eras) {
  struct rs *rs = (struct rs *)p;
#endif
  int deg_lambda, el, deg_omega;
  int i, j, r, k;
  DTYPE u, q, tmp, num1, num2, den, discr_r;
  DTYPE lambda[NROOTS + 1], s[NROOTS];
  DTYPE b[NROOTS + 1], t[NROOTS + 1], omega[NROOTS + 1];
  DTYPE root[NROOTS], reg[NROOTS + 1], loc[NROOTS];
  int syn_error, count;

#ifdef FIXED
  if (pad < 0 || pad >= NN)
    return -1;
#endif

  for (i = 0; i < NROOTS; i++)
    s[i] = data[0];

  for (j = 1; j < NN - PAD; j++) {
    for (i = 0; i < NROOTS; i++) {
      if (s[i] == 0) {
        s[i] = data[j];
      } else {
        s[i] = data[j] ^ ALPHA_TO[MODNN(INDEX_OF[s[i]] + (FCR + i) * PRIM)];
      }
    }
  }

  syn_error = 0;
  for (i = 0; i < NROOTS; i++) {
    syn_error |= s[i];
    s[i] = INDEX_OF[s[i]];
  }

  if (!syn_error) {
    count = 0;
    goto finish;
  }

  memset(&lambda[1], 0, NROOTS * sizeof(lambda[0]));
  lambda[0] = 1;

  if (no_eras > 0) {
    lambda[1] = ALPHA_TO[MODNN(PRIM * (NN - 1 - eras_pos[0]))];
    for (i = 1; i < no_eras; i++) {
      u = MODNN(PRIM * (NN - 1 - eras_pos[i]));
      for (j = i + 1; j > 0; j--) {
        tmp = INDEX_OF[lambda[j - 1]];
        if (tmp != A0)
          lambda[j] ^= ALPHA_TO[MODNN(u + tmp)];
      }
    }

#if DEBUG >= 1
    for (i = 1; i <= no_eras; i++)
      reg[i] = INDEX_OF[lambda[i]];

    count = 0;
    for (i = 1, k = IPRIM - 1; i <= NN; i++, k = MODNN(k + IPRIM)) {
      q = 1;
      for (j = 1; j <= no_eras; j++)
        if (reg[j] != A0) {
          reg[j] = MODNN(reg[j] + j);
          q ^= ALPHA_TO[reg[j]];
        }
      if (q != 0)
        continue;
      root[count] = i;
      loc[count] = k;
      count++;
    }
    if (count != no_eras) {
      printf("count = %d no_eras = %d\n lambda(x) is WRONG\n", count, no_eras);
      count = -1;
      goto finish;
    }
#if DEBUG >= 2
    printf("\n Erasure positions as determined by roots of Eras Loc Poly:\n");
    for (i = 0; i < count; i++)
      printf("%d ", loc[i]);
    printf("\n");
#endif
#endif
  }

  for (i = 0; i < NROOTS + 1; i++)
    b[i] = INDEX_OF[lambda[i]];

  r = no_eras;
  el = no_eras;
  while (++r <= NROOTS) {
    discr_r = 0;
    for (i = 0; i < r; i++) {
      if ((lambda[i] != 0) && (s[r - i - 1] != A0)) {
        discr_r ^= ALPHA_TO[MODNN(INDEX_OF[lambda[i]] + s[r - i - 1])];
      }
    }
    discr_r = INDEX_OF[discr_r];
    if (discr_r == A0) {
      memmove(&b[1], b, NROOTS * sizeof(b[0]));
      b[0] = A0;
    } else {
      t[0] = lambda[0];
      for (i = 0; i < NROOTS; i++) {
        if (b[i] != A0)
          t[i + 1] = lambda[i + 1] ^ ALPHA_TO[MODNN(discr_r + b[i])];
        else
          t[i + 1] = lambda[i + 1];
      }
      if (2 * el <= r + no_eras - 1) {
        el = r + no_eras - el;
        for (i = 0; i <= NROOTS; i++)
          b[i] = (lambda[i] == 0) ? A0 : MODNN(INDEX_OF[lambda[i]] - discr_r + NN);
      } else {
        memmove(&b[1], b, NROOTS * sizeof(b[0]));
        b[0] = A0;
      }
      memcpy(lambda, t, (NROOTS + 1) * sizeof(t[0]));
    }
  }

  deg_lambda = 0;
  for (i = 0; i < NROOTS + 1; i++) {
    lambda[i] = INDEX_OF[lambda[i]];
    if (lambda[i] != A0)
      deg_lambda = i;
  }

  memcpy(&reg[1], &lambda[1], NROOTS * sizeof(reg[0]));
  count = 0;
  for (i = 1, k = IPRIM - 1; i <= NN; i++, k = MODNN(k + IPRIM)) {
    q = 1;
    for (j = deg_lambda; j > 0; j--) {
      if (reg[j] != A0) {
        reg[j] = MODNN(reg[j] + j);
        q ^= ALPHA_TO[reg[j]];
      }
    }
    if (q != 0)
      continue;
#if DEBUG >= 2
    printf("count %d root %d loc %d\n", count, i, k);
#endif
    root[count] = i;
    loc[count] = k;
    if (++count == deg_lambda)
      break;
  }
  if (deg_lambda != count) {
    count = -1;
    goto finish;
  }

  deg_omega = deg_lambda - 1;
  for (i = 0; i <= deg_omega; i++) {
    tmp = 0;
    for (j = i; j >= 0; j--) {
      if ((s[i - j] != A0) && (lambda[j] != A0))
        tmp ^= ALPHA_TO[MODNN(s[i - j] + lambda[j])];
    }
    omega[i] = INDEX_OF[tmp];
  }

  for (j = count - 1; j >= 0; j--) {
    num1 = 0;
    for (i = deg_omega; i >= 0; i--) {
      if (omega[i] != A0)
        num1 ^= ALPHA_TO[MODNN(omega[i] + i * root[j])];
    }
    num2 = ALPHA_TO[MODNN(root[j] * (FCR - 1) + NN)];
    den = 0;

    for (i = min(deg_lambda, NROOTS - 1) & ~1; i >= 0; i -= 2) {
      if (lambda[i + 1] != A0)
        den ^= ALPHA_TO[MODNN(lambda[i + 1] + i * root[j])];
    }
#if DEBUG >= 1
    if (den == 0) {
      printf("\n ERROR: denominator = 0\n");
      count = -1;
      goto finish;
    }
#endif
    if (num1 != 0 && loc[j] >= PAD) {
      data[loc[j] - PAD] ^= ALPHA_TO[MODNN(INDEX_OF[num1] + INDEX_OF[num2] + NN - INDEX_OF[den])];
    }
  }

finish:
  if (eras_pos != NULL) {
    for (i = 0; i < count; i++)
      eras_pos[i] = loc[i];
  }
  return count;
}
