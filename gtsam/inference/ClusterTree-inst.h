/**
 * @file ClusterTree-inst.h
 * @date Oct 8, 2013
 * @author Kai Ni
 * @author Richard Roberts
 * @author Frank Dellaert
 * @brief Collects factorgraph fragments defined on variable clusters, arranged in a tree
 */

#include <gtsam/base/timing.h>
#include <gtsam/base/treeTraversal-inst.h>
#include <gtsam/inference/ClusterTree.h>
#include <gtsam/inference/BayesTree.h>
#include <gtsam/inference/Ordering.h>

#include <boost/foreach.hpp>
#include <boost/bind.hpp>

namespace gtsam
{
  namespace
  {
    /* ************************************************************************* */
    // Elimination traversal data - stores a pointer to the parent data and collects the factors
    // resulting from elimination of the children.  Also sets up BayesTree cliques with parent and
    // child pointers.
    template<class CLUSTERTREE>
    struct EliminationData {
      EliminationData* const parentData;
      size_t myIndexInParent;
      FastVector<typename CLUSTERTREE::sharedFactor> childFactors;
      boost::shared_ptr<typename CLUSTERTREE::BayesTreeType::Node> bayesTreeNode;

      // Constructor used when creating a BayesTree during elimination
      EliminationData(EliminationData* _parentData, size_t nChildren) :
        parentData(_parentData),
        bayesTreeNode(boost::make_shared<typename CLUSTERTREE::BayesTreeType::Node>())
      {
        // Allocate space for child factor pointers
        childFactors.reserve(nChildren);

        // Add a null elimination result pointer to our parent and keep track of our index
        if(parentData) {
          myIndexInParent = parentData->childFactors.size();
          parentData->childFactors.push_back(typename CLUSTERTREE::sharedFactor());
        } else {
          myIndexInParent = 0;
        }

        // Set up BayesTree parent and child pointers
        if(parentData) {
          if(parentData->parentData) // If our parent is not the dummy node
            bayesTreeNode->parent_ = parentData->bayesTreeNode;
          parentData->bayesTreeNode->children.push_back(bayesTreeNode);
        }
      }
    };

    /* ************************************************************************* */
    // Elimination traversal data for in-place elimination - stores a pointer to the parent data and collects
    // the factors resulting from elimination of the children. Also sets up BayesTree cliques with parent and
    // child pointers.
    template<class CLUSTERTREE>
    struct EliminationDataInPlace {
      EliminationDataInPlace* const parentData;
      size_t myIndexInParent;
      FastVector<typename CLUSTERTREE::sharedFactor> childFactors;
      typename CLUSTERTREE::BayesTreeType::Node* bayesTreeNode;

      // Constructor used when reusing a BayesTree
      EliminationDataInPlace(EliminationDataInPlace* _parentData, size_t nChildren) :
        parentData(_parentData)
      {
        // Allocate space for child factor pointers
        childFactors.reserve(nChildren);

        // Add a null elimination result pointer to our parent and keep track of our index
        if(parentData) {
          myIndexInParent = parentData->childFactors.size();
          parentData->childFactors.push_back(typename CLUSTERTREE::sharedFactor());
        } else {
          myIndexInParent = 0;
        }

        // Get BayesTree node pointer
        if(parentData)
          bayesTreeNode = parentData->bayesTreeNode->children[myIndexInParent].get();
      }
    };

    /* ************************************************************************* */
    // Elimination pre-order visitor - just creates the EliminationData structure for the visited
    // node.
    template<class CLUSTERTREE, class ELIMINATIONDATA>
    ELIMINATIONDATA eliminationPreOrderVisitor(
      const typename CLUSTERTREE::sharedNode& node, ELIMINATIONDATA& parentData)
    {
      ELIMINATIONDATA myData(&parentData, node->children.size());
      myData.bayesTreeNode->problemSize_ = node->problemSize();
      return myData;
    }

    /* ************************************************************************* */
    // Helper function for the elimination post-order visitor. Performs the tasks common to in-place and
    // non-in-place elimination. Combines the child factors with our own factors, sets the result in the
    // BayesTree node, and adds the remaining factor to the parent.
    template<class CLUSTERTREE, class ELIMINATIONDATA>
    void eliminationPostOrderVisitorHelper(const typename CLUSTERTREE::Eliminate& eliminationFunction,
                                           const typename CLUSTERTREE::sharedNode& node,
                                           ELIMINATIONDATA& myData) {

      // Typedefs
      typedef typename CLUSTERTREE::FactorType FactorType;
      typedef typename CLUSTERTREE::FactorGraphType FactorGraphType;
      typedef typename CLUSTERTREE::ConditionalType ConditionalType;

      // Gather factors
      FactorGraphType gatheredFactors;
      gatheredFactors.reserve(node->factors.size() + node->children.size());
      gatheredFactors += node->factors;
      gatheredFactors += myData.childFactors;

      // Do dense elimination step
      std::pair<boost::shared_ptr<ConditionalType>, boost::shared_ptr<FactorType> > eliminationResult =
        eliminationFunction(gatheredFactors, Ordering(node->keys));

      // Store conditional in BayesTree clique, and in the case of ISAM2Clique also store the remaining factor
      myData.bayesTreeNode->setEliminationResult(eliminationResult);

      // Store remaining factor in parent's gathered factors
      if(!eliminationResult.second->empty())
        myData.parentData->childFactors[myData.myIndexInParent] = eliminationResult.second;
    }

    /* ************************************************************************* */
    // Elimination post-order visitor - combine the child factors with our own factors, add the
    // resulting conditional to the BayesTree, and add the remaining factor to the parent.
    template<class CLUSTERTREE>
    struct EliminationPostOrderVisitor
    {
      const typename CLUSTERTREE::Eliminate& eliminationFunction;
      typename CLUSTERTREE::BayesTreeType::Nodes& nodesIndex;

      EliminationPostOrderVisitor(const typename CLUSTERTREE::Eliminate& eliminationFunction,
        typename CLUSTERTREE::BayesTreeType::Nodes& nodesIndex) :
      eliminationFunction(eliminationFunction), nodesIndex(nodesIndex) {}

      void operator()(const typename CLUSTERTREE::sharedNode& node, EliminationData<CLUSTERTREE>& myData)
      {
        typedef typename CLUSTERTREE::sharedFactor sharedFactor;
        typedef typename CLUSTERTREE::BayesTreeType::Node BTNode;

        // Call helper function to do the elimination work
        eliminationPostOrderVisitorHelper<CLUSTERTREE, EliminationData<CLUSTERTREE> >(eliminationFunction, node, myData);

        // Check for Bayes tree orphan subtrees, and add them to our children
        BOOST_FOREACH(const sharedFactor& f, node->factors)
        {
          if(const BayesTreeOrphanWrapper<BTNode>* asSubtree = dynamic_cast<const BayesTreeOrphanWrapper<BTNode>*>(f.get()))
          {
            myData.bayesTreeNode->children.push_back(asSubtree->clique);
            asSubtree->clique->parent_ = myData.bayesTreeNode;
          }
        }

        // Fill nodes index - we do this here instead of calling insertRoot at the end to avoid
        // putting orphan subtrees in the index - they'll already be in the index of the ISAM2
        // object they're added to.
        BOOST_FOREACH(const Key& j, myData.bayesTreeNode->conditional()->frontals())
          nodesIndex.insert(std::make_pair(j, myData.bayesTreeNode));
      }
    };

    /* ************************************************************************* */
    // Elimination post-order visitor - combine the child factors with our own factors, add the
    // resulting conditional to the BayesTree, and add the remaining factor to the parent.
    template<class CLUSTERTREE>
    struct EliminationPostOrderVisitorInPlace
    {
      const typename CLUSTERTREE::Eliminate& eliminationFunction;

      EliminationPostOrderVisitorInPlace(const typename CLUSTERTREE::Eliminate& eliminationFunction) :
      eliminationFunction(eliminationFunction) {}

      void operator()(const typename CLUSTERTREE::sharedNode& node, EliminationDataInPlace<CLUSTERTREE>& myData)
      {
        typedef typename CLUSTERTREE::sharedFactor sharedFactor;
        typedef typename CLUSTERTREE::BayesTreeType::Node BTNode;

        // Check for Bayes tree orphan subtrees, which are not supported for in-place elimination
        BOOST_FOREACH(const sharedFactor& f, node->factors)
        {
          if(dynamic_cast<const BayesTreeOrphanWrapper<BTNode>*>(f.get()))
          {
            throw RuntimeErrorThreadsafe("Encountered a BayesTreeOrphanWrapper while doing in-place elimination,\n"
                                         "which is not supported.  BayesTreeOrphanWrapper is normally only created\n"
                                         "internally, so this may be caused by creating BayesTreeOrphanWrapper\n"
                                         "externally to GTSAM.");
          }
        }

        // Call helper function to do the elimination work
        eliminationPostOrderVisitorHelper<CLUSTERTREE, EliminationDataInPlace<CLUSTERTREE> >(eliminationFunction, node, myData);
      }
    };
  }

  /* ************************************************************************* */
  template<class BAYESTREE, class GRAPH>
  void ClusterTree<BAYESTREE,GRAPH>::Cluster::print(
    const std::string& s, const KeyFormatter& keyFormatter) const
  {
    std::cout << s;
    BOOST_FOREACH(Key j, keys)
      std::cout << keyFormatter(j) << "  ";
    std::cout << "problemSize = " << problemSize_ << std::endl;
  }

  /* ************************************************************************* */
  template<class BAYESTREE, class GRAPH>
  void ClusterTree<BAYESTREE,GRAPH>::print(
    const std::string& s, const KeyFormatter& keyFormatter) const
  {
    treeTraversal::PrintForest(*this, s, keyFormatter);
  }

  /* ************************************************************************* */
  template<class BAYESTREE, class GRAPH>
  ClusterTree<BAYESTREE,GRAPH>& ClusterTree<BAYESTREE,GRAPH>::operator=(const This& other)
  {
    // Start by duplicating the tree.
    roots_ = treeTraversal::CloneForest(other);

    // Assign the remaining factors - these are pointers to factors in the original factor graph and
    // we do not clone them.
    remainingFactors_ = other.remainingFactors_;

    return *this;
  }

  /* ************************************************************************* */
  template<class BAYESTREE, class GRAPH>
  std::pair<boost::shared_ptr<BAYESTREE>, boost::shared_ptr<GRAPH> >
  ClusterTree<BAYESTREE,GRAPH>::eliminate(const Eliminate& function) const
  {
    gttic(ClusterTree_eliminate);

    static __itt_domain* fctree = 0;
    if(fctree == 0) {
      fctree =  __itt_domain_create("CTree eliminate");
      fctree->flags = 1;
    }

    __itt_frame_begin_v3(fctree, NULL);

    // Do elimination (depth-first traversal).  The rootsContainer stores a 'dummy' BayesTree node
    // that contains all of the roots as its children.  rootsContainer also stores the remaining
    // uneliminated factors passed up from the roots.
    boost::shared_ptr<BayesTreeType> result = boost::make_shared<BayesTreeType>();
    EliminationData<This> rootsContainer(0, roots_.size());
    EliminationPostOrderVisitor<This> visitorPost(function, result->nodes_);
    {
      TbbOpenMPMixedScope threadLimiter; // Limits OpenMP threads since we're mixing TBB and OpenMP
      treeTraversal::DepthFirstForestParallel(*this, rootsContainer,
        eliminationPreOrderVisitor<This, EliminationData<This> >, visitorPost, 10);
    }

    // Create BayesTree from roots stored in the dummy BayesTree node.
    result->roots_.insert(result->roots_.end(), rootsContainer.bayesTreeNode->children.begin(), rootsContainer.bayesTreeNode->children.end());

    // Add remaining factors that were not involved with eliminated variables
    boost::shared_ptr<FactorGraphType> allRemainingFactors = boost::make_shared<FactorGraphType>();
    allRemainingFactors->reserve(remainingFactors_.size() + rootsContainer.childFactors.size());
    allRemainingFactors->push_back(remainingFactors_.begin(), remainingFactors_.end());
    BOOST_FOREACH(const sharedFactor& factor, rootsContainer.childFactors)
      if(factor)
        allRemainingFactors->push_back(factor);

    __itt_frame_end_v3(fctree, NULL);

    // Return result
    return std::make_pair(result, allRemainingFactors);
  }

  /* ************************************************************************* */
  template<class BAYESTREE, class GRAPH>
  boost::shared_ptr<GRAPH>
  ClusterTree<BAYESTREE,GRAPH>::eliminateInPlace(BAYESTREE& bayesTree, const Eliminate& function) const
  {
    gttic(ClusterTree_eliminateInPlace);

    static __itt_domain* fctree = 0;
    if(fctree == 0) {
      fctree =  __itt_domain_create("CTree eliminate");
      fctree->flags = 1;
    }

    __itt_frame_begin_v3(fctree, NULL);
    // Do elimination (depth-first traversal).  The rootsContainer stores a 'dummy' BayesTree node
    // that contains all of the roots as its children.  rootsContainer also stores the remaining
    // uneliminated factors passed up from the roots.
    EliminationDataInPlace<This> rootsContainer(0, roots_.size());
    typename BAYESTREE::Node rootsBayesTreeNode;
    rootsContainer.bayesTreeNode = &rootsBayesTreeNode;
    rootsContainer.bayesTreeNode->children = bayesTree.roots();

    EliminationPostOrderVisitorInPlace<This> visitorPost(function);
    {
      TbbOpenMPMixedScope threadLimiter; // Limits OpenMP threads since we're mixing TBB and OpenMP
      treeTraversal::DepthFirstForestParallel(*this, rootsContainer,
        eliminationPreOrderVisitor<This, EliminationDataInPlace<This> >, visitorPost, 10);
    }

    // Add remaining factors that were not involved with eliminated variables
    boost::shared_ptr<FactorGraphType> allRemainingFactors = boost::make_shared<FactorGraphType>();
    allRemainingFactors->reserve(remainingFactors_.size() + rootsContainer.childFactors.size());
    allRemainingFactors->push_back(remainingFactors_.begin(), remainingFactors_.end());
    BOOST_FOREACH(const sharedFactor& factor, rootsContainer.childFactors)
      if(factor)
        allRemainingFactors->push_back(factor);

    __itt_frame_end_v3(fctree, NULL);

    // Return remaining factors
    return allRemainingFactors;
  }

}
