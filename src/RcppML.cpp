// This file is part of RcppML, an Rcpp Machine Learning library
//
// Copyright (C) 2021 Zach DeBruine <zacharydebruine@gmail.com>
// github.com/zdebruine/RcppML

#define EIGEN_NO_DEBUG
#define EIGEN_INITIALIZE_MATRICES_BY_ZERO

//[[Rcpp::plugins(openmp)]]
#ifdef _OPENMP
#include <omp.h>
#endif

//[[Rcpp::depends(RcppEigen)]]
#include <RcppEigen.h>

// CONTENTS
// section 1: helper subroutines
// section 2: non-negative least squares
// section 3: Rcpp dgCMatrix matrix class
// section 4: projecting linear factor models
// section 5: mean squared error loss of a factor model
// section 6: matrix factorization by alternating least squares
// section 7: Rcpp wrapper functions

// ********************************************************************************
// SECTION 1: Helper subroutines
// ********************************************************************************

// these functions for matrix subsetting are documented here:
// http://eigen.tuxfamily.org/dox-devel/TopicCustomizing_NullaryExpr.html#title1
// official support will likely appear in Eigen 4.0, this is a patch in the meantime
template<class ArgType, class RowIndexType, class ColIndexType>
class indexing_functor {
  const ArgType& m_arg;
  const RowIndexType& m_rowIndices;
  const ColIndexType& m_colIndices;
public:
  typedef Eigen::Matrix<typename ArgType::Scalar,
    RowIndexType::SizeAtCompileTime,
    ColIndexType::SizeAtCompileTime,
    ArgType::Flags& Eigen::RowMajorBit ? Eigen::RowMajor : Eigen::ColMajor,
    RowIndexType::MaxSizeAtCompileTime,
    ColIndexType::MaxSizeAtCompileTime> MatrixType;

  indexing_functor(const ArgType& arg, const RowIndexType& row_indices, const ColIndexType& col_indices)
    : m_arg(arg), m_rowIndices(row_indices), m_colIndices(col_indices) {}

  const typename ArgType::Scalar& operator() (Eigen::Index row, Eigen::Index col) const {
    return m_arg(m_rowIndices[row], m_colIndices[col]);
  }
};

template <class ArgType, class RowIndexType, class ColIndexType>
Eigen::CwiseNullaryOp<indexing_functor<ArgType, RowIndexType, ColIndexType>,
  typename indexing_functor<ArgType, RowIndexType, ColIndexType>::MatrixType>
  submat(const Eigen::MatrixBase<ArgType>& arg, const RowIndexType& row_indices, const ColIndexType& col_indices) {
  typedef indexing_functor<ArgType, RowIndexType, ColIndexType> Func;
  typedef typename Func::MatrixType MatrixType;
  return MatrixType::NullaryExpr(row_indices.size(), col_indices.size(), Func(arg.derived(), row_indices, col_indices));
}

// calculate sort index of vector "d" in decreasing order
std::vector<int> sort_index(const Eigen::VectorXd& d) {
  std::vector<int> idx(d.size());
  std::iota(idx.begin(), idx.end(), 0);
  sort(idx.begin(), idx.end(), [&d](size_t i1, size_t i2) {return d[i1] > d[i2];});
  return idx;
}

// reorder rows in dynamic matrix "x" by integer vector "ind"
Eigen::MatrixXd reorder_rows(const Eigen::MatrixXd& x, const std::vector<int>& ind) {
  Eigen::MatrixXd x_reordered(x.rows(), x.cols());
  for (unsigned int i = 0; i < ind.size(); ++i)
    x_reordered.row(i) = x.row(ind[i]);
  return x_reordered;
}

// reorder elements in vector "x" by integer vector "ind"
Eigen::VectorXd reorder(const Eigen::VectorXd& x, const std::vector<int>& ind) {
  Eigen::VectorXd x_reordered(x.size());
  for (unsigned int i = 0; i < ind.size(); ++i)
    x_reordered(i) = x(ind[i]);
  return x_reordered;
}

// get indices of values in "x" greater than zero
template<typename Derived>
Eigen::VectorXi find_gtz(const Eigen::MatrixBase<Derived>& x) {
  unsigned int n_gtz = 0;
  for (unsigned int i = 0; i < x.size(); ++i) if (x[i] > 0) ++n_gtz;
  Eigen::VectorXi gtz(n_gtz);
  unsigned int j = 0;
  for (unsigned int i = 0; i < x.size(); ++i) {
    if (x[i] > 0) {
      gtz[j] = i;
      ++j;
    }
  }
  return gtz;
}

double cor(Eigen::MatrixXd& x, Eigen::MatrixXd& y) {
  double x_i, y_i, sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0, sum_y2 = 0;
  const unsigned int n = x.size();
  for (unsigned int i = 0; i < n; ++i) {
    x_i = (*(x.data() + i));
    y_i = (*(y.data() + i));
    sum_x += x_i;
    sum_y += y_i;
    sum_xy += x_i * y_i;
    sum_x2 += x_i * x_i;
    sum_y2 += y_i * y_i;
  }
  return 1 - (n * sum_xy - sum_x * sum_y) / std::sqrt((n * sum_x2 - sum_x * sum_x) * (n * sum_y2 - sum_y * sum_y));
}

// ********************************************************************************
// SECTION 2: Non-negative least squares
// ********************************************************************************

// coordinate descent (CD) least squares given an initial 'x'
template <typename Derived>
Eigen::VectorXd c_cdnnls(
  const Eigen::MatrixXd& a,
  const typename Eigen::MatrixBase<Derived>& b,
  Eigen::VectorXd x,
  const unsigned int cd_maxit,
  const double cd_tol,
  const bool nonneg) {

  Eigen::VectorXd b0 = a * x;
  for (unsigned int i = 0; i < b0.size(); ++i) b0(i) -= b(i);
  double cd_tol_xi, cd_tol_it = 1 + cd_tol;
  for (unsigned int it = 0; it < cd_maxit && cd_tol_it > cd_tol; ++it) {
    cd_tol_it = 0;
    for (unsigned int i = 0; i < x.size(); ++i) {
      double xi = x(i) - b0(i) / a(i, i);
      if (nonneg && xi < 0) xi = 0;
      if (xi != x(i)) {
        // update gradient
        b0 += a.col(i) * (xi - x(i));
        // calculate tolerance for this value, update iteration tolerance if needed
        cd_tol_xi = 2 * std::abs(x(i) - xi) / (xi + x(i) + 1e-16);
        if (cd_tol_xi > cd_tol_it) cd_tol_it = cd_tol_xi;
        x(i) = xi;
      }
    }
  }
  return x;
}

// Fast active set tuning (FAST) least squares given only 'a', 'b', and a pre-conditioned LLT decomposition of 'a'
template<typename Derived>
Eigen::VectorXd c_nnls(
  const Eigen::MatrixXd& a,
  const typename Eigen::MatrixBase<Derived>& b,
  const Eigen::LLT<Eigen::MatrixXd, 1>& a_llt,
  const unsigned int fast_maxit,
  const unsigned int cd_maxit,
  const double cd_tol,
  const bool nonneg) {

  // initialize with unconstrained least squares solution
  Eigen::VectorXd x = a_llt.solve(b);
  // iterative feasible set reduction while unconstrained least squares solutions at feasible indices contain negative values
  for (unsigned int it = 0; nonneg && it < fast_maxit && (x.array() < 0).any(); ++it) {
    // get indices in "x" greater than zero (the "feasible set")
    Eigen::VectorXi gtz_ind = find_gtz(x);
    // subset "a" and "b" to those indices in the feasible set
    Eigen::VectorXd bsub(gtz_ind.size());
    for (unsigned int i = 0; i < gtz_ind.size(); ++i) bsub(i) = b(gtz_ind(i));
    Eigen::MatrixXd asub = submat(a, gtz_ind, gtz_ind);
    // solve for those indices in "x"
    Eigen::VectorXd xsub = asub.llt().solve(bsub);
    x.setZero();
    for (unsigned int i = 0; i < gtz_ind.size(); ++i) x(gtz_ind(i)) = xsub(i);
  }

  if (cd_maxit == 0 && nonneg) return x;
  else return c_cdnnls(a, b, x, cd_maxit, cd_tol, nonneg);
}

// ********************************************************************************
// SECTION 3: Rcpp dgCMatrix class
// ********************************************************************************

// zero-copy sparse matrix class for access by reference to R objects already in memory
// note that Eigen::SparseMatrix<double> requires a deep copy of R objects for use in C++

namespace Rcpp {
  class dgCMatrix {
  public:
    Rcpp::IntegerVector i, p, Dim;
    Rcpp::NumericVector x;

    // Constructor from Rcpp::S4 object
    dgCMatrix(Rcpp::S4 m) : i(m.slot("i")), p(m.slot("p")), Dim(m.slot("Dim")), x(m.slot("x")) {}

    int rows() { return Dim[0]; }
    int cols() { return Dim[1]; }

    class InnerIterator {
    public:
      InnerIterator(dgCMatrix& ptr, int col) : ptr(ptr) { index = ptr.p[col]; max_index = ptr.p[col + 1]; }
      operator bool() const { return (index < max_index); }
      InnerIterator& operator++() { ++index; return *this; }
      const double& value() const { return ptr.x[index]; }
      int row() const { return ptr.i[index]; }
    private:
      dgCMatrix& ptr;
      int index, max_index;
    };
  };
}

// ********************************************************************************
// SECTION 4: Projecting linear factor models
// ********************************************************************************

// solve the least squares equation A = wh for h given A and w

Eigen::MatrixXd c_project_sparse(
  Rcpp::dgCMatrix& A,
  Eigen::MatrixXd& w,
  const bool nonneg,
  const unsigned int fast_maxit,
  const unsigned int cd_maxit,
  const double cd_tol,
  const double L1,
  const unsigned int threads) {

  // "w" must be in "wide" format, where w.cols() == A.rows()

  // compute left-hand side of linear system, "a"
  Eigen::MatrixXd a = w * w.transpose();
  for(unsigned int i = 0; i < a.cols(); ++i) a(i, i) += 1e-15;
  Eigen::LLT<Eigen::MatrixXd, 1> a_llt = a.llt();
  unsigned int k = a.rows();
  Eigen::MatrixXd h(k, A.cols());

  #pragma omp parallel for num_threads(threads) schedule(dynamic)
  for (unsigned int i = 0; i < A.cols(); ++i) {
    // compute right-hand side of linear system, "b", in-place in h
    Eigen::VectorXd b = Eigen::VectorXd::Zero(k);
    for (Rcpp::dgCMatrix::InnerIterator it(A, i); it; ++it)
      for (unsigned int j = 0; j < k; ++j) b(j) += it.value() * w(j, it.row());

    if (L1 != 0) for (unsigned int j = 0; j < k; ++j) b(j) -= L1;

    // solve least squares
    h.col(i) = c_nnls(a, b, a_llt, fast_maxit, cd_maxit, cd_tol, nonneg);
  }
  return h;
}

//[[Rcpp::export]]
Eigen::MatrixXd Rcpp_project_dense(
    const Rcpp::NumericMatrix& A,
    Eigen::MatrixXd& w,
    const bool nonneg,
    const unsigned int fast_maxit,
    const unsigned int cd_maxit,
    const double cd_tol,
    const double L1,
    const unsigned int threads) {
  
  Eigen::MatrixXd a = w * w.transpose();
  for(unsigned int i = 0; i < a.cols(); ++i) a(i, i) += 1e-15;
  Eigen::LLT<Eigen::MatrixXd, 1> a_llt = a.llt();
  unsigned int k = a.rows();
  Eigen::MatrixXd h(k, A.cols());
  
  #pragma omp parallel for num_threads(threads) schedule(dynamic)
  for (unsigned int i = 0; i < A.cols(); ++i) {
    // compute right-hand side of linear system, "b", in-place in h
    Eigen::VectorXd b = Eigen::VectorXd::Zero(k);
    for (unsigned int it = 0; it < A.rows(); ++it)
      b += A(it, i) * w.col(it);
    if (L1 != 0) for (unsigned int j = 0; j < k; ++j) b(j) -= L1;
    h.col(i) = c_nnls(a, b, a_llt, fast_maxit, cd_maxit, cd_tol, nonneg);
  }
  return h;
}

// ********************************************************************************
// SECTION 5: Mean squared error loss of a matrix factorization model
// ********************************************************************************

//[[Rcpp::export]]
double Rcpp_mse_sparse(
  const Rcpp::S4& A_S4,
  Eigen::MatrixXd w,
  const Eigen::VectorXd& d,
  const Eigen::MatrixXd& h,
  const unsigned int threads) {

  Rcpp::dgCMatrix A(A_S4);
  if (w.rows() == h.rows()) w.transposeInPlace();

  // multiply w by diagonal
  #pragma omp parallel for num_threads(threads) schedule(dynamic)
  for (unsigned int i = 0; i < w.cols(); ++i)
    for (unsigned int j = 0; j < w.rows(); ++j)
      w(j, i) *= d(i);

  // calculate total loss with parallelization across samples
  Eigen::VectorXd losses(A.cols());
  losses.setZero();
  #pragma omp parallel for num_threads(threads) schedule(dynamic)
  for (unsigned int i = 0; i < A.cols(); ++i) {
    Eigen::VectorXd wh_i = w * h.col(i);
    for (Rcpp::dgCMatrix::InnerIterator iter(A, i); iter; ++iter)
      wh_i(iter.row()) -= iter.value();
    for (unsigned int j = 0; j < wh_i.size(); ++j)
      losses(i) += std::pow(wh_i(j), 2);
  }
  return losses.sum() / (A.cols() * A.rows());
}

//[[Rcpp::export]]
double Rcpp_mse_dense(
  const Rcpp::NumericMatrix& A,
  Eigen::MatrixXd w,
  const Eigen::VectorXd& d,
  const Eigen::MatrixXd& h,
  const unsigned int threads) {

  if (w.rows() == h.rows()) w.transposeInPlace();

  // multiply w by diagonal
  #pragma omp parallel for num_threads(threads) schedule(dynamic)
  for (unsigned int i = 0; i < w.cols(); ++i)
    for (unsigned int j = 0; j < w.rows(); ++j)
      w(j, i) *= d(i);

  // calculate total loss with parallelization across samples
  Eigen::VectorXd losses(A.cols());
  losses.setZero();
  #pragma omp parallel for num_threads(threads) schedule(dynamic)
  for (unsigned int i = 0; i < A.cols(); ++i) {
    Eigen::VectorXd wh_i = w * h.col(i);
    for(unsigned int iter = 0; iter < A.rows(); ++iter)
      wh_i(iter) -= A(iter, i);      
    for (unsigned int j = 0; j < wh_i.size(); ++j)
      losses(i) += std::pow(wh_i(j), 2);
  }
  return losses.sum() / (A.cols() * A.rows());
}

// ********************************************************************************
// SECTION 6: Matrix Factorization by Alternating Least Squares
// ********************************************************************************

//[[Rcpp::export]]
Rcpp::List Rcpp_nmf_sparse(
  const Rcpp::S4& A_S4,
  const Rcpp::S4& At_S4,
  const bool symmetric,
  Eigen::MatrixXd w,
  const double tol = 1e-3,
  const bool nonneg = true,
  const double L1_w = 0,
  const double L1_h = 0,
  const unsigned int maxit = 100,
  const bool diag = true,
  const unsigned int fast_maxit = 10,
  const unsigned int cd_maxit = 100,
  const double cd_tol = 1e-8,
  const bool verbose = false,
  const unsigned int threads = 0) {

  Rcpp::checkUserInterrupt();
  unsigned int k = w.rows();
  Rcpp::dgCMatrix A(A_S4), At(At_S4);
  Eigen::MatrixXd h(k, A.cols());
  Eigen::VectorXd d(k);
  d = d.setOnes();
  double tol_ = 1;
  unsigned int it;

  if (verbose) Rprintf("\n%4s | %8s \n---------------\n", "iter", "tol");

  for (it = 0; it < maxit; ++it) {
    // update h
    h = c_project_sparse(A, w, nonneg, fast_maxit, cd_maxit, cd_tol, L1_h, threads);
    Rcpp::checkUserInterrupt();

    // reset diagonal and scale "h"
    for (unsigned int i = 0; diag && i < k; ++i) {
      d[i] = h.row(i).sum() + 1e-15;
      for (unsigned int j = 0; j < A.cols(); ++j) h(i, j) /= d(i);
    }

    // update w
    Eigen::MatrixXd w_it = w;
    if (symmetric) w = c_project_sparse(A, h, nonneg, fast_maxit, cd_maxit, cd_tol, L1_w, threads);
    else w = c_project_sparse(At, h, nonneg, fast_maxit, cd_maxit, cd_tol, L1_w, threads);
    Rcpp::checkUserInterrupt();

    // reset diagonal and scale "w"
    for (unsigned int i = 0; diag && i < k; ++i) {
      d[i] = w.row(i).sum() + 1e-15;
      for (unsigned int j = 0; j < A.rows(); ++j) w(i, j) /= d(i);
    }

    // calculate tolerance
    tol_ = cor(w, w_it);
    if (verbose) Rprintf("%4d | %8.2e\n", it + 1, tol_);

    // check for convergence
    if (tol_ < tol) break;
  }

  // reorder factors by diagonal
  if (diag) {
    std::vector<int> indx = sort_index(d);
    w = reorder_rows(w, indx);
    d = reorder(d, indx);
    h = reorder_rows(h, indx);
  }

  return Rcpp::List::create(Rcpp::Named("w") = w.transpose(), Rcpp::Named("d") = d, Rcpp::Named("h") = h, Rcpp::Named("tol") = tol_, Rcpp::Named("iter") = it);
}

//[[Rcpp::export]]
Rcpp::List Rcpp_nmf_dense(
  const Rcpp::NumericMatrix& A,
  const bool symmetric,
  Eigen::MatrixXd w,
  const double tol = 1e-3,
  const bool nonneg = true,
  const double L1_w = 0,
  const double L1_h = 0,
  const unsigned int maxit = 100,
  const bool diag = true,
  const unsigned int fast_maxit = 10,
  const unsigned int cd_maxit = 100,
  const double cd_tol = 1e-8,
  const bool verbose = false,
  const unsigned int threads = 0) {

  Rcpp::checkUserInterrupt();
  unsigned int k = w.rows();
  Rcpp::NumericMatrix At;
  if(!symmetric) At = Rcpp::transpose(A);
  Eigen::MatrixXd h(k, A.cols());
  Eigen::VectorXd d(k);
  d = d.setOnes();
  double tol_ = 1;
  unsigned int it;

  if (verbose) Rprintf("\n%4s | %8s \n---------------\n", "iter", "tol");

  for (it = 0; it < maxit; ++it) {
    // update h
    h = Rcpp_project_dense(A, w, nonneg, fast_maxit, cd_maxit, cd_tol, L1_h, threads);
    Rcpp::checkUserInterrupt();

    // reset diagonal and scale "h"
    for (unsigned int i = 0; diag && i < k; ++i) {
      d[i] = h.row(i).sum() + 1e-15;
      for (unsigned int j = 0; j < A.cols(); ++j) h(i, j) /= d(i);
    }

    // update w
    Eigen::MatrixXd w_it = w;
    if (symmetric) w = Rcpp_project_dense(A, h, nonneg, fast_maxit, cd_maxit, cd_tol, L1_w, threads);
    else w = Rcpp_project_dense(At, h, nonneg, fast_maxit, cd_maxit, cd_tol, L1_w, threads);
    Rcpp::checkUserInterrupt();

    // reset diagonal and scale "w"
    for (unsigned int i = 0; diag && i < k; ++i) {
      d[i] = w.row(i).sum() + 1e-15;
      for (unsigned int j = 0; j < A.rows(); ++j) w(i, j) /= d(i);
    }

    // calculate tolerance
    tol_ = cor(w, w_it);
    if (verbose) Rprintf("%4d | %8.2e\n", it + 1, tol_);

    // check for convergence
    if (tol_ < tol) break;
  }

  // reorder factors by diagonal
  if (diag) {
    std::vector<int> indx = sort_index(d);
    w = reorder_rows(w, indx);
    d = reorder(d, indx);
    h = reorder_rows(h, indx);
  }

  return Rcpp::List::create(Rcpp::Named("w") = w.transpose(), Rcpp::Named("d") = d, Rcpp::Named("h") = h, Rcpp::Named("tol") = tol_, Rcpp::Named("iter") = it);
}

//[[Rcpp::export]]
Rcpp::List Rcpp_nmf2_sparse(
  const Rcpp::S4& A_S4,
  Eigen::MatrixXd h,
  const double tol,
  const bool nonneg,
  const unsigned int maxit,
  const bool verbose,
  const bool diag) {

  Rcpp::checkUserInterrupt();
  Rcpp::dgCMatrix A(A_S4);
  Eigen::MatrixXd w(2, A.rows()), wb(2, A.rows());
  Eigen::Vector2d d = Eigen::Vector2d::Ones();
  double tol_ = 1;
  unsigned int it;

  if (verbose) Rprintf("\n%4s | %8s \n---------------\n", "iter", "tol");

  for (it = 0; it < maxit; ++it) {
    // update w
    Eigen::Matrix2d a = h * h.transpose();
    double denom = a(0, 0) * a(1, 1) - a(0, 1) * a(0, 1);
    wb.setZero();
    for (unsigned int i = 0; i < A.cols(); ++i) {
      // compute right-hand side of linear system, "b", in-place in h
      for (Rcpp::dgCMatrix::InnerIterator it(A, i); it; ++it){
        wb(0, it.row()) += it.value() * h(0, i);
        wb(1, it.row()) += it.value() * h(1, i);
      }
    }

    for (unsigned int i = 0; i < A.rows(); ++i) {
      // solve least squares
      if (nonneg) {
        const double a01b1 = a(0, 1) * wb(1, i);
        const double a11b0 = a(1, 1) * wb(0, i);
        if (a11b0 < a01b1) {
            w(0, i) = 0;
            w(1, i) = wb(1, i) / a(1, 1);
        } else {
            const double a01b0 = a(0, 1) * wb(0, i);
            const double a00b1 = a(0, 0) * wb(1, i);
            if (a00b1 < a01b0) {
                w(0, i) = wb(0, i) / a(0, 0);
                w(1, i) = 0;
            } else {
                w(0, i) = (a11b0 - a01b1) / denom;
                w(1, i) = (a00b1 - a01b0) / denom;
            }
        }
      } else {
        w(0, i) = (a(1, 1) * wb(0, i) - a(0, 1) * wb(1, i)) / denom;
        w(1, i) = (a(0, 0) * wb(1, i) - a(0, 1) * wb(0, i)) / denom;
      }
    }

    // reset diagonal and scale "w"
    for (unsigned int i = 0; diag && i < 2; ++i) {
      d[i] = w.row(i).sum() + 1e-15;
      for (unsigned int j = 0; j < A.rows(); ++j) w(i, j) /= d(i);
    }

    // update h
    Eigen::MatrixXd h_it = h;
    a = w * w.transpose();
    denom = a(0, 0) * a(1, 1) - a(0, 1) * a(0, 1);
    for (unsigned int i = 0; i < A.cols(); ++i) {
      // compute right-hand side of linear system, "b", in-place in h
      double b0 = 0, b1 = 0;
      for (Rcpp::dgCMatrix::InnerIterator it(A, i); it; ++it){
        b0 += it.value() * w(0, it.row());
        b1 += it.value() * w(1, it.row());
      }

      // solve least squares
      if (nonneg) {
        const double a01b1 = a(0, 1) * b1;
        const double a11b0 = a(1, 1) * b0;
        if (a11b0 < a01b1) {
            h(0, i) = 0;
            h(1, i) = b1 / a(1, 1);
        } else {
            const double a01b0 = a(0, 1) * b0;
            const double a00b1 = a(0, 0) * b1;
            if (a00b1 < a01b0) {
                h(0, i) = b0 / a(0, 0);
                h(1, i) = 0;
            } else {
                h(0, i) = (a11b0 - a01b1) / denom;
                h(1, i) = (a00b1 - a01b0) / denom;
            }
        }
      } else {
        h(0, i) = (a(1, 1) * b0 - a(0, 1) * b1) / denom;
        h(1, i) = (a(0, 0) * b1 - a(0, 1) * b0) / denom;
      }
    }

    // reset diagonal and scale "h"
    for (unsigned int i = 0; diag && i < 2; ++i) {
      d[i] = h.row(i).sum() + 1e-15;
      for (unsigned int j = 0; j < A.cols(); ++j) h(i, j) /= d(i);
    }

    // calculate tolerance
    tol_ = cor(h, h_it);
    if (verbose) Rprintf("%4d | %8.2e\n", it + 1, tol_);
    Rcpp::checkUserInterrupt();

    // check for convergence
    if (tol_ < tol) break;
  }

  // sort factors by diagonal value
  if (diag && d(0) < d(1)) {
    w.col(0).swap(w.row(0));
    h.col(0).swap(h.row(0));
    const double d1 = d(1); d(1) = d(0); d(0) = d1;
  }

  return Rcpp::List::create(Rcpp::Named("w") = w.transpose(), Rcpp::Named("d") = d, Rcpp::Named("h") = h, Rcpp::Named("tol") = tol_, Rcpp::Named("iter") = it);
}

//[[Rcpp::export]]
Rcpp::List Rcpp_nmf2_dense(
  const Rcpp::NumericMatrix& A,
  Eigen::MatrixXd h,
  const double tol,
  const bool nonneg,
  const unsigned int maxit,
  const bool verbose,
  const bool diag) {

  Rcpp::checkUserInterrupt();
  Eigen::MatrixXd w(2, A.rows()), wb(2, A.rows());
  Eigen::Vector2d d = Eigen::Vector2d::Ones();
  double tol_ = 1;
  unsigned int it;

  if (verbose) Rprintf("\n%4s | %8s \n---------------\n", "iter", "tol");

  for (it = 0; it < maxit; ++it) {
    // update w
    Eigen::Matrix2d a = h * h.transpose();
    double denom = a(0, 0) * a(1, 1) - a(0, 1) * a(0, 1);
    wb.setZero();
    for (unsigned int i = 0; i < A.cols(); ++i) {
      // compute right-hand side of linear system, "b", in-place in h
      for (unsigned int it = 0; it < A.rows(); ++it){
        wb(0, it) += A(it, i) * h(0, i);
        wb(1, it) += A(it, i) * h(1, i);
      }
    }

    for (unsigned int i = 0; i < A.rows(); ++i) {
      // solve least squares
      if (nonneg) {
        const double a01b1 = a(0, 1) * wb(1, i);
        const double a11b0 = a(1, 1) * wb(0, i);
        if (a11b0 < a01b1) {
            w(0, i) = 0;
            w(1, i) = wb(1, i) / a(1, 1);
        } else {
            const double a01b0 = a(0, 1) * wb(0, i);
            const double a00b1 = a(0, 0) * wb(1, i);
            if (a00b1 < a01b0) {
                w(0, i) = wb(0, i) / a(0, 0);
                w(1, i) = 0;
            } else {
                w(0, i) = (a11b0 - a01b1) / denom;
                w(1, i) = (a00b1 - a01b0) / denom;
            }
        }
      } else {
        w(0, i) = (a(1, 1) * wb(0, i) - a(0, 1) * wb(1, i)) / denom;
        w(1, i) = (a(0, 0) * wb(1, i) - a(0, 1) * wb(0, i)) / denom;
      }
    }

    // reset diagonal and scale "w"
    for (unsigned int i = 0; diag && i < 2; ++i) {
      d[i] = w.row(i).sum() + 1e-15;
      for (unsigned int j = 0; j < A.rows(); ++j) w(i, j) /= d(i);
    }

    // update h
    Eigen::MatrixXd h_it = h;
    a = w * w.transpose();
    denom = a(0, 0) * a(1, 1) - a(0, 1) * a(0, 1);
    for (unsigned int i = 0; i < A.cols(); ++i) {
      // compute right-hand side of linear system, "b", in-place in h
      double b0 = 0, b1 = 0;
      for (unsigned int it = 0; it < A.rows(); ++it){
        b0 += A(it, i) * w(0, it);
        b1 += A(it, i) * w(1, it);
      }

      // solve least squares
      if (nonneg) {
        const double a01b1 = a(0, 1) * b1;
        const double a11b0 = a(1, 1) * b0;
        if (a11b0 < a01b1) {
            h(0, i) = 0;
            h(1, i) = b1 / a(1, 1);
        } else {
            const double a01b0 = a(0, 1) * b0;
            const double a00b1 = a(0, 0) * b1;
            if (a00b1 < a01b0) {
                h(0, i) = b0 / a(0, 0);
                h(1, i) = 0;
            } else {
                h(0, i) = (a11b0 - a01b1) / denom;
                h(1, i) = (a00b1 - a01b0) / denom;
            }
        }
      } else {
        h(0, i) = (a(1, 1) * b0 - a(0, 1) * b1) / denom;
        h(1, i) = (a(0, 0) * b1 - a(0, 1) * b0) / denom;
      }
    }

    // reset diagonal and scale "h"
    for (unsigned int i = 0; diag && i < 2; ++i) {
      d[i] = h.row(i).sum() + 1e-15;
      for (unsigned int j = 0; j < A.cols(); ++j) h(i, j) /= d(i);
    }

    // calculate tolerance
    tol_ = cor(h, h_it);
    if (verbose) Rprintf("%4d | %8.2e\n", it + 1, tol_);
    Rcpp::checkUserInterrupt();

    // check for convergence
    if (tol_ < tol) break;
  }

  // sort factors by diagonal value
  if (diag && d(0) < d(1)) {
    w.col(0).swap(w.row(0));
    h.col(0).swap(h.row(0));
    const double d1 = d(1); d(1) = d(0); d(0) = d1;
  }

  return Rcpp::List::create(Rcpp::Named("w") = w.transpose(), Rcpp::Named("d") = d, Rcpp::Named("h") = h, Rcpp::Named("tol") = tol_, Rcpp::Named("iter") = it);
}

// ********************************************************************************
// SECTION 7: Rcpp wrapper functions when needed
// ********************************************************************************

// Non-Negative Least Squares

//[[Rcpp::export]]
Eigen::MatrixXd Rcpp_nnls(
  const Eigen::MatrixXd& a,
  Eigen::MatrixXd b,
  const unsigned int fast_maxit = 10,
  const unsigned int cd_maxit = 100,
  const double cd_tol = 1e-8,
  const bool nonneg = true) {

  Eigen::LLT<Eigen::MatrixXd> a_llt = a.llt();
  for (unsigned int i = 0; i < b.cols(); ++i)
    b.col(i) = c_nnls(a, b.col(i), a.llt(), fast_maxit, cd_maxit, cd_tol, nonneg);
  return b;
}

//[[Rcpp::export]]
Eigen::MatrixXd Rcpp_cdnnls(
  const Eigen::MatrixXd& a,
  Eigen::MatrixXd& b,
  Eigen::MatrixXd x,
  const unsigned int cd_maxit = 100,
  const double cd_tol = 1e-8,
  const bool nonneg = true) {

  for (unsigned int i = 0; i < b.cols(); ++i) {
    Eigen::VectorXd x_i = x.col(i);
    x.col(i) = c_cdnnls(a, b.col(i), x_i, cd_maxit, cd_tol, nonneg);
  }
  return x;
}

//[[Rcpp::export]]
Eigen::MatrixXd Rcpp_project_sparse(
  const Rcpp::S4& A_S4,
  Eigen::MatrixXd& w,
  const bool nonneg,
  const unsigned int fast_maxit,
  const unsigned int cd_maxit,
  const double cd_tol,
  const double L1,
  const unsigned int threads) {

  Rcpp::dgCMatrix A(A_S4);
  return c_project_sparse(A, w, nonneg, fast_maxit, cd_maxit, cd_tol, L1, threads);
}