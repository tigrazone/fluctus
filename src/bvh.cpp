#include <iostream>
#include <cfloat>
#include "bvh.hpp"

BVH::BVH(std::vector<RTTriangle>* tris, SplitMode mode) {
	m_triangles = tris;
	m_mode = mode;

	// Init index list with 0...N
	m_indices = std::vector<U32>(m_triangles->size());
	std::iota(m_indices.begin(), m_indices.end(), 0);
	
	BuildNode root = BuildNode();
	root.iStart = 0;
	root.iEnd = (U32)m_triangles->size() - 1;
	m_build_nodes.push_back(root);
	nodes++;
	
	build(0, 0); // root, depth 0

	createSmallNodes();
	
	std::cout 
		<< "======================" << std::endl
		<< ((mode == SplitMode_Sah) ? "SAH" : (mode == SplitMode_ObjectMedian) ? "Object Median" : (mode == SplitMode_SpatialMedian) ? "Spatial Median" : "Unknown") << std::endl
		<< "Splits: " << metrics.splits << " (" << int(metrics.bad_splits/float(metrics.splits) * 100.0f) << "% bad)" << std::endl
		<< "Depth: " << metrics.depth << std::endl
		<< "Leaves: " << metrics.leaves << std::endl
		<< "======================" << std::endl;
}

BVH::BVH(std::vector<RTTriangle>* tris, const char* filename) {
	m_triangles = tris;
	importFrom(filename);
}

void BVH::createSmallNodes() {
	for (BuildNode bn : m_build_nodes) {
		Node n;
		n.box = bn.box;
		if (bn.rightChild == -1) { // leaf node
			n.iStart = bn.iStart;
			U32 sp = bn.spannedTris();
			if (sp > 255) {
				throw std::runtime_error("Too many prims to fit into U8!");
			}
			n.nPrims = (U8)(sp);
		}
		else { // interior node
			n.rightChild = (U32)(bn.rightChild);
		}
		m_nodes.push_back(n);
	}
}

std::vector<U32> importIndices(std::ifstream &in) {
	U32 size;
	read(in, size);
	std::vector<U32> vec(size);
	
	for (U32 i = 0; i < size; i++) {
		U32 ind;
		read(in, ind);
		vec[i] = ind;
	}

	// Return value optimization!
	return vec;
}

std::vector<BuildNode> importNodes(std::ifstream &in) {
	U32 size;
	read(in, size);
	std::vector<BuildNode> vec(size);
	
	for (U32 i = 0; i < size; i++) {
		BuildNode n;
		float3 bmin, bmax;
		read(in, bmin.x);
		read(in, bmin.y);
		read(in, bmin.z);
		read(in, bmax.x);
		read(in, bmax.y);
		read(in, bmax.z);
		n.box = AABB { bmin, bmax };
		read(in, n.iStart);
		read(in, n.iEnd);
		read(in, n.rightChild);
		vec[i] = n;
	}

	// Return value optimization!
	return vec;
}


void BVH::importFrom(const char* filename) {
	std::ifstream infile(filename, std::ios::binary);
	m_indices = importIndices(infile);
	m_build_nodes = importNodes(infile);
	createSmallNodes();
}


void exportNode(std::ofstream &out, const BuildNode &n) {
	// AABB
	float3 bmin = n.box.min;
	float3 bmax = n.box.max;
	write(out, (F32)bmin.x);
	write(out, (F32)bmin.y);
	write(out, (F32)bmin.z);
	write(out, (F32)bmax.x);
	write(out, (F32)bmax.y);
	write(out, (F32)bmax.z);

	// Parameters
	write(out, (U32)n.iStart);
	write(out, (U32)n.iEnd);
	write(out, (S32)n.rightChild);
}

/** Write BVH to file for later importing **/
void BVH::exportTo(const char* filename) const {
	std::ofstream out(filename, std::ios::binary);

	// Index list
	write(out, (U32)m_indices.size());
	for_each(m_indices.begin(), m_indices.end(), [&out](U32 index) { write(out, index); });

	// Node list
	write(out, (U32)m_nodes.size());
	for_each(m_build_nodes.begin(), m_build_nodes.end(), [&out](const BuildNode &n) { exportNode(out, n); });
}

void BVH::build(U32 nInd, U32 depth) {
	m_build_nodes[nInd].computeBB(m_indices, m_triangles);
	metrics.depth = std::max(metrics.depth, depth);
	U32 elems = m_build_nodes[nInd].spannedTris();
	if (elems > MAX_LEAF_ELEMS) {
		U32 split;
		if(!sortElems(m_build_nodes[nInd], split)) { // parent is cheaper
			metrics.leaves++;
			return;
		}

		metrics.splits++;

		// Left child
		BuildNode leftChild = BuildNode();
		leftChild.iStart = m_build_nodes[nInd].iStart;
		leftChild.iEnd = split - 1;
		m_build_nodes.push_back(leftChild);
		nodes++;
		build(nodes - 1, ++depth); // last pushed
		
		// Right child
		BuildNode rightChild = BuildNode();
		rightChild.iStart = split;
		rightChild.iEnd = m_build_nodes[nInd].iEnd;
		m_build_nodes.push_back(rightChild);
		m_build_nodes[nInd].rightChild = nodes++;
		build(nodes - 1, ++depth);
	}
	else {
		metrics.leaves++;
	}
}

// The maximum length of any vector within the scene.
// Needed as the vector length determines the maximum intersection distance.
F32 BVH::sceneDiagLen() const {
	Node root = m_nodes[0];
	return length(root.box.max - root.box.min);
}

bool BVH::sortElems(BuildNode &n, U32 &split) {
	switch (m_mode) {
		case SplitMode_Sah:
			return sahSplit(n, split);
			break;
		case SplitMode_SpatialMedian:
			return spatialMedianSplit(n, split);
			break;
		case SplitMode_ObjectMedian:
			return objectMedianSplit(n, split);
			break;
		default:
			std::cout << "Selected split mode not implemented!" << std::endl;
			throw std::runtime_error("Selected split mode not implemented!");
			break;
	}
}

bool BVH::objectMedianSplit(BuildNode &n, U32 &split) {
	return objectMedianSplit(n, n.box.maxDim(), split);
}

bool BVH::objectMedianSplit(BuildNode &n, U32 dim, U32 &split) {
	auto s = m_indices.begin() + n.iStart;
	auto e = m_indices.begin() + n.iEnd;

	// Sort the range [s, e[
	std::sort(s, e, [this, dim](U32 &i1, U32 &i2) { return (*this->m_triangles)[i1].centroid()[dim] < (*this->m_triangles)[i2].centroid()[dim]; });

	split = n.iStart + n.spannedTris() / 2;
	return true;
}

// Use centroids to get split position in spatial median split (reduces bad split amount)
inline F32 BVH::centroidSplit(U32 iStart, U32 iEnd, U32 dim) {
	F32 pmin = (*m_triangles)[m_indices[iStart]].centroid()[dim];
	F32 pmax = pmin;
	for (U32 i = iStart + 1; i <= iEnd; i++) {
		F32 comp = (*m_triangles)[m_indices[i]].centroid()[dim];
		pmin = std::min(comp, pmin);
		pmax = std::max(comp, pmax);
	}
	
	return 0.5f * (pmin + pmax);
}

bool BVH::spatialMedianSplit(BuildNode &n, U32 &split) {
	U32 dim = n.box.maxDim(); // index of longest axis
	F32 splitCoord = centroidSplit(n.iStart, n.iEnd, dim);

	auto s = m_indices.begin() + n.iStart;
	auto e = m_indices.begin() + n.iEnd;

	// Partition the range [s, e[
	auto it = std::partition(s, e, [this, dim, splitCoord](U32 &i) { return (*this->m_triangles)[i].centroid()[dim] < splitCoord; });

	split = (U32)(it - m_indices.begin()); // Index of split point in index list (first elem. of second group)

	// Fix bad splits (use midpoint)
	if (split == n.iStart || split == n.iEnd) {
		metrics.bad_splits++;
		objectMedianSplit(n, split);
	}

	return true;
}

inline F32 BVH::sahCost(U32 N1, F32 area1, U32 N2, F32 area2, F32 area_root) const {
	F32 lcost = N1 * area1 / area_root;
	F32 rcost = N2 * area2 / area_root;
	return 2 * sahParams.costBox + sahParams.costTri * (lcost + rcost);
}

// lookup[n] = area of AABB with last n + 1 triangles
std::vector<F32> BVH::buildAreaLookup(BuildNode &n) const {
	std::vector<F32> vec;
	AABB box;
	for (U32 i = 0; i < n.spannedTris(); i++) {
		RTTriangle t = (*m_triangles)[m_indices[n.iEnd - i]];
		box.expand(t);
		vec.push_back(box.area());
	}
	return vec;
}

bool BVH::sahSplit(BuildNode &n, U32 &split) {
	F32 parentArea = n.box.area();
	F32 parentCost = sahParams.costBox + n.spannedTris() * sahParams.costTri;
	F32 bestCost = FLT_MAX;
	
	U32 bestSplitPoint;
	U32 bestAxis;

	// Loop over all three axes to find best split
	for (U32 dim = 0; dim < 3; dim++) {
		// Sort along axis
		objectMedianSplit(n, dim, split);

		// Create area lookup array
		std::vector<F32> rightAreas = buildAreaLookup(n);

		AABB leftBox;
		U32 leftCount = 0;
		U32 spanSize = n.spannedTris();

		// Try different split points along axis
		for (U32 s = n.iStart; s < n.iEnd; s++) { // exclude last (all on left side)
			leftBox.expand((*m_triangles)[m_indices[s]]);
			leftCount++;

			F32 areaLeft = leftBox.area();
			F32 areaRight = rightAreas[n.iEnd - s - 1];

			// New best split?
			F32 cost = sahCost(leftCount, areaLeft,
				spanSize - leftCount, areaRight, parentArea);

			if (cost < bestCost) {
				bestCost = cost;
				bestSplitPoint = s;
				bestAxis = dim;
			}
		} // END LOOP SPLIT POINTS
	} // END LOOP AXES

	// Worse than parent?
	if (bestCost > parentCost) {
		return false;
	}

	// Re-sort along best axis, if necessary
	if (bestAxis != 2) {
		objectMedianSplit(n, bestAxis, split);
	}

	// Fix indexing if only one triangle on either side
	if (bestSplitPoint == n.iStart) {
		bestSplitPoint++;
		metrics.bad_splits++;
	} else if (bestSplitPoint == n.iEnd) {
		bestSplitPoint--;
		metrics.bad_splits++;
	}

	split = bestSplitPoint;
	return true;
}


/*bool BVH::intersect(RaycastResult& res, const float3& orig, const float3& dir, bool occlusion) const {
	return recurse(res, orig, 1.0f / dir, occlusion);
}*/

/*bool BVH::recurse(RaycastResult& res, const float3& orig, const float3& invDir, bool occlusion) const {
	float lnear, lfar, rnear, rfar; //AABB limits
	U32 closer, farther;

	// Stack state
	SimStackNode stack[64];
	S32 stackptr = 0;

	// Root node
	stack[stackptr].i = 0;
	stack[stackptr].mint = -FLT_MAX;

	while (stackptr >= 0) {
		// Next node
		int ni = stack[stackptr].i;
		float tnear = stack[stackptr].mint;
		stackptr--;
		const Node n = m_nodes[ni];

		// Closer intersection found already
		if (tnear > res.t)
			continue;

		if (n.nPrims != 0) { // Leaf node
			float3 dir = 1.0f / invDir;
			float tmin = 1.0f, umin = 0.0f, vmin = 0.0f;
			int imin = -1;
			for (U32 i = n.iStart; i < n.iStart + n.nPrims; i++) {
				float t, u, v;
				RTTriangle tri = (*m_triangles)[m_indices[i]];
				if (tri.intersect_woop(orig, dir, t, u, v)) {
					if (t > 0.0f && t < tmin && tri.isOpaque(u, v)) {
						imin = i;
						tmin = t;
						umin = u;
						vmin = v;

						if (occlusion) {
							res = RaycastResult(&(*m_triangles)[m_indices[imin]], tmin, umin, vmin, orig + tmin*dir, orig, dir);
							return true;
						}
					}
				}
			}
			if (imin != -1 && tmin < res.t) {
				res = RaycastResult(&(*m_triangles)[m_indices[imin]], tmin, umin, vmin, orig + tmin*dir, orig, dir);
			}
		}
		else { // Internal node
			bool leftWasHit = m_nodes[ni + 1].box.intersectSlab(orig, invDir, &lnear, &lfar);
			bool rightWasHit = m_nodes[n.rightChild].box.intersectSlab(orig, invDir, &rnear, &rfar);

			if (leftWasHit && rightWasHit) {
				closer = ni + 1;
				farther = n.rightChild;

				// Right child was closer -> swap
				if (rnear < lnear) {
					std::swap(lnear, rnear);
					std::swap(lfar, rfar);
					std::swap(closer, farther);
				}

				// Farther node pushed first
				stack[++stackptr] = SimStackNode(farther, rnear);
				stack[++stackptr] = SimStackNode(closer, lnear);
			}

			else if (leftWasHit) {
				stack[++stackptr] = SimStackNode(ni + 1, lnear);
			}

			else if (rightWasHit) {
				stack[++stackptr] = SimStackNode(n.rightChild, rnear);
			}
		}
	}

	return (res.tri != nullptr);
}*/