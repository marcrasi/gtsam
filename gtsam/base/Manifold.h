/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file Manifold.h
 * @brief Base class and basic functions for Manifold types
 * @author Alex Cunningham
 * @author Frank Dellaert
 * @author Mike Bosse
 */

#pragma once

#include <gtsam/base/Matrix.h>
#include <gtsam/base/Testable.h>
#include <gtsam/base/OptionalJacobian.h>

#include <boost/concept_check.hpp>
#include <boost/concept/requires.hpp>
#include <boost/type_traits/is_base_of.hpp>

namespace gtsam {

/// tag to assert a type is a manifold
struct manifold_tag {};

/**
 * A manifold defines a space in which there is a notion of a linear tangent space
 * that can be centered around a given point on the manifold.  These nonlinear
 * spaces may have such properties as wrapping around (as is the case with rotations),
 * which might make linear operations on parameters not return a viable element of
 * the manifold.
 *
 * We perform optimization by computing a linear delta in the tangent space of the
 * current estimate, and then apply this change using a retraction operation, which
 * maps the change in tangent space back to the manifold itself.
 *
 * There may be multiple possible retractions for a given manifold, which can be chosen
 * between depending on the computational complexity.  The important criteria for
 * the creation for the retract and localCoordinates functions is that they be
 * inverse operations.
 *
 */

template <typename T> struct traits;

namespace internal {

/// Requirements on type to pass it to Manifold template below
template<class Class>
struct HasManifoldPrereqs {

  enum { dim = Class::dimension };

  Class p, q;
  Eigen::Matrix<double, dim, 1> v;
  OptionalJacobian<dim, dim> Hp, Hq, Hv;

  BOOST_CONCEPT_USAGE(HasManifoldPrereqs) {
    v = p.localCoordinates(q);
    q = p.retract(v);
  }
};

/// Extra manifold traits for fixed-dimension types
template<class Class, int N>
struct ManifoldImpl {
  // Compile-time dimensionality
  static int GetDimension(const Class&) {
    return N;
  }
};

/// Extra manifold traits for variable-dimension types
template<class Class>
struct ManifoldImpl<Class, Eigen::Dynamic> {
  // Run-time dimensionality
  static int GetDimension(const Class& m) {
    return m.dim();
  }
};

/// A helper that implements the traits interface for GTSAM manifolds.
/// To use this for your class type, define:
/// template<> struct traits<Class> : public internal::ManifoldTraits<Class> { };
template<class Class>
struct ManifoldTraits: ManifoldImpl<Class, Class::dimension> {

  // Check that Class has the necessary machinery
  BOOST_CONCEPT_ASSERT((HasManifoldPrereqs<Class>));

  // Dimension of the manifold
  enum { dimension = Class::dimension };

  // Typedefs required by all manifold types.
  typedef Class ManifoldType;
  typedef manifold_tag structure_category;
  typedef Eigen::Matrix<double, dimension, 1> TangentVector;

  // Local coordinates
  static TangentVector Local(const Class& origin, const Class& other) {
    return origin.localCoordinates(other);
  }

  // Retraction back to manifold
  static Class Retract(const Class& origin, const TangentVector& v) {
    return origin.retract(v);
  }
};

/// Both ManifoldTraits and Testable
template<class Class> struct Manifold: ManifoldTraits<Class>, Testable<Class> {};

} // \ namespace internal

/// Check invariants for Manifold type
template<typename T>
BOOST_CONCEPT_REQUIRES(((IsTestable<T>)),(bool)) //
check_manifold_invariants(const T& a, const T& b, double tol=1e-9) {
  typename traits<T>::TangentVector v0 = traits<T>::Local(a,a);
  typename traits<T>::TangentVector v = traits<T>::Local(a,b);
  T c = traits<T>::Retract(a,v);
  return v0.norm() < tol && traits<T>::Equals(b,c,tol);
}

/// Manifold concept
template<typename T>
class IsManifold {

public:

  typedef typename traits<T>::structure_category structure_category_tag;
  static const int dim = traits<T>::dimension;
  typedef typename traits<T>::ManifoldType ManifoldType;
  typedef typename traits<T>::TangentVector TangentVector;

  BOOST_CONCEPT_USAGE(IsManifold) {
    BOOST_STATIC_ASSERT_MSG(
        (boost::is_base_of<manifold_tag, structure_category_tag>::value),
        "This type's structure_category trait does not assert it as a manifold (or derived)");
    BOOST_STATIC_ASSERT(TangentVector::SizeAtCompileTime == dim);

    // make sure Chart methods are defined
    v = traits<T>::Local(p, q);
    q = traits<T>::Retract(p, v);
  }

private:

  TangentVector v;
  ManifoldType p, q;
};

/// Give fixed size dimension of a type, fails at compile time if dynamic
template<typename T>
struct FixedDimension {
  typedef const int value_type;
  static const int value = traits<T>::dimension;
  BOOST_STATIC_ASSERT_MSG(value != Eigen::Dynamic,
      "FixedDimension instantiated for dymanically-sized type.");
};

#ifdef GTSAM_ALLOW_DEPRECATED_SINCE_V4
/// Helper class to construct the product manifold of two other manifolds, M1 and M2
/// Deprecated because of limited usefulness, maximum obfuscation
template<typename M1, typename M2>
class ProductManifold: public std::pair<M1, M2> {
  BOOST_CONCEPT_ASSERT((IsManifold<M1>));
  BOOST_CONCEPT_ASSERT((IsManifold<M2>));

protected:
  enum { dimension1 = traits<M1>::dimension };
  enum { dimension2 = traits<M2>::dimension };

public:
  enum { dimension = dimension1 + dimension2 };
  inline static size_t Dim() { return dimension;}
  inline size_t dim() const { return dimension;}

  typedef Eigen::Matrix<double, dimension, 1> TangentVector;
  typedef OptionalJacobian<dimension, dimension> ChartJacobian;

  /// Default constructor needs default constructors to be defined
  ProductManifold():std::pair<M1,M2>(M1(),M2()) {}

  // Construct from two original manifold values
  ProductManifold(const M1& m1, const M2& m2):std::pair<M1,M2>(m1,m2) {}

  /// Retract delta to manifold
  ProductManifold retract(const TangentVector& xi) const {
    M1 m1 = traits<M1>::Retract(this->first,  xi.template head<dimension1>());
    M2 m2 = traits<M2>::Retract(this->second, xi.template tail<dimension2>());
    return ProductManifold(m1,m2);
  }

  /// Compute the coordinates in the tangent space
  TangentVector localCoordinates(const ProductManifold& other) const {
    typename traits<M1>::TangentVector v1 = traits<M1>::Local(this->first,  other.first);
    typename traits<M2>::TangentVector v2 = traits<M2>::Local(this->second, other.second);
    TangentVector v;
    v << v1, v2;
    return v;
  }

  // Alignment, see https://eigen.tuxfamily.org/dox/group__TopicStructHavingEigenMembers.html
  enum { NeedsToAlign = (sizeof(M1) % 16) == 0 || (sizeof(M2) % 16) == 0
  };
public:
	EIGEN_MAKE_ALIGNED_OPERATOR_NEW_IF(NeedsToAlign)
};

// Define any direct product group to be a model of the multiplicative Group concept
template<typename M1, typename M2>
struct traits<ProductManifold<M1, M2> > : internal::Manifold<ProductManifold<M1, M2> > {
};
#endif

} // \ namespace gtsam

///**
// * Macros for using the ManifoldConcept
// *  - An instantiation for use inside unit tests
// *  - A typedef for use inside generic algorithms
// *
// * NOTE: intentionally not in the gtsam namespace to allow for classes not in
// * the gtsam namespace to be more easily enforced as testable
// */
#define GTSAM_CONCEPT_MANIFOLD_INST(T) template class gtsam::IsManifold<T>;
#define GTSAM_CONCEPT_MANIFOLD_TYPE(T) typedef gtsam::IsManifold<T> _gtsam_IsManifold_##T;
