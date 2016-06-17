/*
 * irlb: A basic C implementation of the implicitly restarted Lanczos
 * bidiagonalization method.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <assert.h>
#include <math.h>

#include <R.h>
#define USE_RINTERNALS
#include <Rinternals.h>
#include <Rdefines.h>

#include "R_ext/BLAS.h"
#include "R_ext/Lapack.h"

#include "irlb.h"

void F77_NAME (dgesvd) (const char *jobu, const char *jobvt, const int *m,
                        const int *n, double *a, const int *lda, double *s,
                        double *u, const int *ldu, double *vt,
                        const int *ldvt, double *work, const int *lwork,
                        int *info);


SEXP
IRLB (SEXP X, SEXP NU, SEXP INIT, SEXP WORK, SEXP MAXIT, SEXP TOL, SEXP EPS)
{
  SEXP ANS, S, U, V;
  double *V1, *U1, *W, *F, *B, *BU, *BV, *BS, *BW, *res, *T;
  int i, iter, mprod, ret;

  double *A = REAL (X);
  int m = nrows (X);
  int n = ncols (X);
  int nu = INTEGER (NU)[0];
  int work = INTEGER (WORK)[0];
  int maxit = INTEGER (MAXIT)[0];
  double tol = REAL (TOL)[0];
  int lwork = 7 * work;
  double eps = REAL (EPS)[0];

  /* ANS = {return code, num. iter, num. matrix vector prodcuts, s, u, v} */
  PROTECT (ANS = NEW_LIST (6));
  PROTECT (S = allocVector (REALSXP, nu));
  PROTECT (U = allocVector (REALSXP, m * work));
  PROTECT (V = allocVector (REALSXP, n * work));
  for (i = 0; i < m; ++i)
    (REAL (V))[i] = (REAL (INIT))[i];

  /* set up intermediate working storage */
  V1 = (double *) R_alloc (n * work, sizeof (double));
  U1 = (double *) R_alloc (m * work, sizeof (double));
  W = (double *) R_alloc (m * work, sizeof (double));
  F = (double *) R_alloc (n, sizeof (double));
  B = (double *) R_alloc (work * work, sizeof (double));
  BU = (double *) R_alloc (work * work, sizeof (double));
  BV = (double *) R_alloc (work * work, sizeof (double));
  BS = (double *) R_alloc (work, sizeof (double));
  BW = (double *) R_alloc (lwork * lwork, sizeof (double));
  res = (double *) R_alloc (work, sizeof (double));
  T = (double *) R_alloc (lwork, sizeof (double));

  ret =
    irlb (A, m, n, nu, work, maxit, tol, REAL (S), REAL (U), REAL (V), &iter,
          &mprod, eps, lwork, V1, U1, W, F, B, BU, BV, BS, BW, res, T);
  SET_VECTOR_ELT (ANS, 0, ScalarInteger (ret));
  SET_VECTOR_ELT (ANS, 1, ScalarInteger (iter));
  SET_VECTOR_ELT (ANS, 2, ScalarInteger (mprod));
  SET_VECTOR_ELT (ANS, 3, S);
  SET_VECTOR_ELT (ANS, 4, U);
  SET_VECTOR_ELT (ANS, 5, V);
  UNPROTECT (4);
  return ANS;
}

/* irlb: main computation function.
 * returns:
 * 0 on success,
 * -1 on misc error
 * -2 not converged
 * -3 out of memory
 * -4 starting vector near the null space of A
 * -5 other linear dependence error
 *
 * all data must be allocated by caller, in particular
 * s must be at least nu * sizeof(double)
 * U must be at least m * work * sizeof(double)
 * V must be at least n * work * sizeof(double)
 * V1  (n * work, sizeof (double))
 * U1  (m * work, sizeof (double))
 * W  (m * work, sizeof (double))
 * F  (n, sizeof (double))
 * B  (work * work, sizeof (double))
 * BU  (work * work, sizeof (double))
 * BV  (work * work, sizeof (double))
 * BS  (work, sizeof (double))
 * BW  (lwork * lwork, sizeof (double))
 * res  (work, sizeof (double))
 * T (lwork, sizeof(double))
 */
int
irlb (double *A,                // Input data matrix
      int m,                    // data matrix number of rows, must be > 3.
      int n,                    // data matrix number of columns, must be > 3.
      int nu,                   // dimension of solution
      int work,                 // working dimension, must be > 3.
      int maxit,                // maximum number of main iterations
      double tol,               // convergence tolerance
      double *s,                // output singular vectors at least length nu
      double *U,                // output left singular vectors  m x work
      double *V,                // output right singular vectors n x work
      int *ITER,                // ouput number of Lanczos iterations
      int *MPROD,               // output number of matrix vector products
      double eps,               // machine epsilon
      // working intermediate storage
      int lwork,
      double *V1,
      double *U1,
      double *W,
      double *F,
      double *B,
      double *BU, double *BV, double *BS, double *BW, double *res, double *T)
{
  double d, S, R, alpha, beta, R_F, SS;
  int jj, kk;
  int converged;
  int info, j, k = 0;
  int inc = 1;
  int retval = -3;
  int mprod = 0;
  int iter = 0;
  double Smax = 0;

/* Check for valid input dimensions */
  if (work < 4 || n < 4 || m < 4)
    return -1;

  memset (B, 0, work * work * sizeof (double));
/* Main iteration */
  while (iter < maxit)
    {
      j = 0;
/*  Normalize starting vector */
      if (iter == 0)
        {
          d = F77_NAME (dnrm2) (&n, V, &inc);
          if (d < 2 * eps)
            return -1;
          d = 1 / d;
          F77_NAME (dscal) (&n, &d, V, &inc);
        }
      else
        j = k;
/*
 * Lanczos bidiagonalization iteration
 * Compute the Lanczos bidiagonal decomposition:
 * AV  = WB
 * t(A)W = VB + Ft(E)
 * with full reorthogonalization.
 */
      alpha = 1;
      beta = 0;
      F77_NAME (dgemm) ("n", "n", &m, &inc, &n, &alpha, A, &m, V + j * n, &n,
                        &beta, W + j * m, &m);
      mprod++;

      if (iter > 0)
        {
/* Orthogonalize jth column of W with previous j columns */
          orthog (W, W + j * m, T, m, j, 1);
        }

      S = F77_NAME (dnrm2) (&m, W + j * m, &inc);
      if (S < tol && j == 1)
        return -4;
      if (S < eps)
        return -5;
      SS = 1.0 / S;
      F77_NAME (dscal) (&m, &SS, W + j * m, &inc);

/* The Lanczos process */
      while (j < work)
        {
          alpha = 1.0;
          beta = 0.0;
          F77_NAME (dgemm) ("t", "n", &n, &inc, &m, &alpha, A, &m, W + j * m,
                            &m, &beta, F, &n);
          mprod++;
          SS = -S;
          F77_NAME (daxpy) (&n, &SS, V + j * n, &inc, F, &inc);
          orthog (V, F, T, n, j + 1, 1);
          R_F = F77_NAME (dnrm2) (&n, F, &inc);
          if (j + 1 < work)
            {
              if (R_F < eps)
                return -5;
              R = 1.0 / R_F;
              memmove (V + (j + 1) * n, F, n * sizeof (double));
              F77_NAME (dscal) (&n, &R, V + (j + 1) * n, &inc);
              B[j * work + j] = S;
              B[(j + 1) * work + j] = R_F;
              alpha = 1.0;
              beta = 0.0;
              F77_NAME (dgemm) ("n", "n", &m, &inc, &n, &alpha, A, &m,
                                V + (j + 1) * n, &n, &beta, W + (j + 1) * m,
                                &m);
              mprod++;
/* One step of classical Gram-Schmidt */
              R = -R_F;
              F77_NAME (daxpy) (&m, &R, W + j * m, &inc, W + (j + 1) * m,
                                &inc);
/* full re-orthogonalization of W */
              if (iter > 1)
                orthog (W, W + (j + 1) * m, T, m, j + 1, 1);
              S = F77_NAME (dnrm2) (&m, W + (j + 1) * m, &inc);
              if (S < eps)
                return -5;
              SS = 1.0 / S;
              F77_NAME (dscal) (&m, &SS, W + (j + 1) * m, &inc);
            }
          else
            {
              B[j * work + j] = S;
            }
          j++;
        }

      memmove (BU, B, work * work * sizeof (double));   // Make a working copy of B
      F77_NAME (dgesvd) ("O", "A", &work, &work, BU, &work, BS, BU, &work, BV,
                         &work, BW, &lwork, &info);
      R = 1.0 / R_F;
      F77_NAME (dscal) (&n, &R, F, &inc);
      for (kk = 0; kk < j; ++kk)
        res[kk] = R_F * BU[kk * work + (j - 1)];

/* Update k to be the number of converged singular values. */
      for (jj = 0; jj < j; ++jj)
        if (BS[jj] > Smax)
          Smax = BS[jj];
      convtests (j, nu, tol, Smax, res, &k, &converged);
      if (converged == 1)
        {
          iter++;
          break;
        }

      alpha = 1;
      beta = 0;
      F77_NAME (dgemm) ("n", "t", &n, &k, &j, &alpha, V, &n, BV, &work, &beta,
                        V1, &n);
      memmove (V, V1, n * k * sizeof (double));
      memmove (V + n * k, F, n * sizeof (double));

      memset (B, 0, work * work * sizeof (double));
      for (jj = 0; jj < k; ++jj)
        {
          B[jj * work + jj] = BS[jj];
          B[k * work + jj] = res[jj];
        }

/*   Update the left approximate singular vectors */
      alpha = 1;
      beta = 0;
      F77_NAME (dgemm) ("n", "n", &m, &k, &j, &alpha, W, &m, BU, &work, &beta,
                        U1, &m);
      memmove (W, U1, m * k * sizeof (double));
      iter++;
    }

/* Results */
  memmove (s, BS, nu * sizeof (double));        /* Singular values */
  alpha = 1;
  beta = 0;
  F77_NAME (dgemm) ("n", "n", &m, &nu, &work, &alpha, W, &m, BU, &work, &beta,
                    U, &m);

  F77_NAME (dgemm) ("n", "t", &n, &nu, &work, &alpha, V, &n, BV, &work, &beta,
                    V1, &n);
  memmove (V, V1, n * nu * sizeof (double));

  *ITER = iter;
  *MPROD = mprod;
  retval = (converged == 1) ? 0 : -2;   // 0 = Success, -2 = not converged.
  return (retval);
}
