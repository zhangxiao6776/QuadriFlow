#include "parametrizer.hpp"
#include "config.hpp"

#include <Eigen/Sparse>
#include <fstream>
#include <list>
#include <queue>
#include "adjacent-matrix.hpp"
#include "dedge.hpp"
#include "field-math.hpp"
#include "flow.hpp"
#include "loader.hpp"
#include "merge-vertex.hpp"
#include "optimizer.hpp"
#include "subdivide.hpp"

//#define LOG_OUTPUT
#define PERFORM_TEST
extern void generate_adjacency_matrix_uniform(const MatrixXi& F, const VectorXi& V2E,
                                              const VectorXi& E2E, const VectorXi& nonManifold,
                                              AdjacentMatrix& adj);

inline double fast_acos(double x) {
    double negate = double(x < 0.0f);
    x = std::abs(x);
    double ret = -0.0187293f;
    ret *= x;
    ret = ret + 0.0742610f;
    ret *= x;
    ret = ret - 0.2121144f;
    ret *= x;
    ret = ret + 1.5707288f;
    ret = ret * std::sqrt(1.0f - x);
    ret = ret - 2.0f * negate * ret;
    return negate * (double)M_PI + ret;
}

void Parametrizer::Load(const char* filename) {
    load(filename, V, F);
    double maxV[3] = {-1e30, -1e30, -1e30};
    double minV[3] = {1e30, 1e30, 1e30};
#ifdef WITH_OMP
#pragma omp parallel for
#endif
    for (int i = 0; i < V.cols(); ++i) {
        for (int j = 0; j < 3; ++j) {
            maxV[j] = std::max(maxV[j], V(j, i));
            minV[j] = std::min(minV[j], V(j, i));
        }
    }
    double scale =
        std::max(std::max(maxV[0] - minV[0], maxV[1] - minV[1]), maxV[2] - minV[2]) * 0.5;
#ifdef WITH_OMP
#pragma omp parallel for
#endif
    for (int i = 0; i < V.cols(); ++i) {
        for (int j = 0; j < 3; ++j) {
            V(j, i) = (V(j, i) - (maxV[j] + minV[j]) * 0.5) / scale;
        }
    }
#ifdef LOG_OUTPUT
    printf("vertices size: %d\n", (int)V.cols());
    printf("faces size: %d\n", (int)F.cols());
#endif

    merge_close(V, F, 1e-6);
}

void Parametrizer::Initialize(int faces, int with_scale) {
    ComputeMeshStatus();
#ifdef PERFORM_TEST
    num_vertices = V.cols() * 10;
    num_faces = num_vertices;
    scale = sqrt(surface_area / num_faces);
#else
    if (faces == -1) {
        num_vertices = V.cols();
        num_faces = num_vertices;
        scale = sqrt(surface_area / num_faces);
    } else {
        double face_area = surface_area / faces;
        num_vertices = faces;
        scale = std::sqrt(face_area) / 2;
    }
#endif
    double target_len = std::min(scale / 2, average_edge_length * 2);
#ifdef PERFORM_TEST
    scale = sqrt(surface_area / V.cols());
#endif
    if (target_len < max_edge_length) {
        compute_direct_graph(V, F, V2E, E2E, boundary, nonManifold);
        subdivide(F, V, V2E, E2E, boundary, nonManifold, target_len);
    }
    int t1 = GetCurrentTime64();
    compute_direct_graph(V, F, V2E, E2E, boundary, nonManifold);
    generate_adjacency_matrix_uniform(F, V2E, E2E, nonManifold, adj);

    ComputeSmoothNormal();
    ComputeVertexArea();

    if (with_scale) {
        triangle_space.resize(F.cols());
#ifdef WITH_OMP
#pragma omp parallel for
#endif
        for (int i = 0; i < F.cols(); ++i) {
            Matrix3d p, q;
            p.col(0) = V.col(F(1, i)) - V.col(F(0, i));
            p.col(1) = V.col(F(2, i)) - V.col(F(0, i));
            p.col(2) = Nf.col(i);
            q = p.inverse();
            triangle_space[i].resize(2, 3);
            for (int j = 0; j < 2; ++j) {
                for (int k = 0; k < 3; ++k) {
                    triangle_space[i](j, k) = q(j, k);
                }
            }
        }
    }
#ifdef LOG_OUTPUT
    printf("V: %d F: %d\n", (int)V.cols(), (int)F.cols());
#endif
    hierarchy.mA[0] = std::move(A);
    hierarchy.mAdj[0] = std::move(adj);
    hierarchy.mN[0] = std::move(N);
    hierarchy.mV[0] = std::move(V);
    hierarchy.mE2E = std::move(E2E);
    hierarchy.mF = std::move(F);
    hierarchy.Initialize(scale, with_scale);
    int t2 = GetCurrentTime64();
    printf("Initialize use time: %lf\n", (t2 - t1) * 1e-3);
}

void Parametrizer::ComputeMeshStatus() {
    surface_area = 0;
    average_edge_length = 0;
    max_edge_length = 0;
    for (int f = 0; f < F.cols(); ++f) {
        Vector3d v[3] = {V.col(F(0, f)), V.col(F(1, f)), V.col(F(2, f))};
        double area = 0.5f * (v[1] - v[0]).cross(v[2] - v[0]).norm();
        surface_area += area;
        for (int i = 0; i < 3; ++i) {
            double len = (v[(i + 1) % 3] - v[i]).norm();
            average_edge_length += len;
            if (len > max_edge_length) max_edge_length = len;
        }
    }
    average_edge_length /= (F.cols() * 3);
}

void Parametrizer::ComputeSmoothNormal() {
    /* Compute face normals */
    Nf.resize(3, F.cols());
#ifdef WITH_OMP
#pragma omp parallel for
#endif
    for (int f = 0; f < F.cols(); ++f) {
        Vector3d v0 = V.col(F(0, f)), v1 = V.col(F(1, f)), v2 = V.col(F(2, f)),
                 n = (v1 - v0).cross(v2 - v0);
        double norm = n.norm();
        if (norm < RCPOVERFLOW) {
            n = Vector3d::UnitX();
        } else {
            n /= norm;
        }
        Nf.col(f) = n;
    }

    N.resize(3, V.cols());
#ifdef WITH_OMP
#pragma omp parallel for
#endif
    for (int i = 0; i < V2E.rows(); ++i) {
        int edge = V2E[i];
        if (nonManifold[i] || edge == -1) {
            N.col(i) = Vector3d::UnitX();
            continue;
        }

        int stop = edge;
        Vector3d normal = Vector3d::Zero();
        do {
            int idx = edge % 3;

            Vector3d d0 = V.col(F((idx + 1) % 3, edge / 3)) - V.col(i);
            Vector3d d1 = V.col(F((idx + 2) % 3, edge / 3)) - V.col(i);
            double angle = fast_acos(d0.dot(d1) / std::sqrt(d0.squaredNorm() * d1.squaredNorm()));

            /* "Computing Vertex Normals from Polygonal Facets"
            by Grit Thuermer and Charles A. Wuethrich, JGT 1998, Vol 3 */
            if (std::isfinite(angle)) normal += Nf.col(edge / 3) * angle;

            int opp = E2E[edge];
            if (opp == -1) break;

            edge = dedge_next_3(opp);
        } while (edge != stop);
        double norm = normal.norm();
        N.col(i) = norm > RCPOVERFLOW ? Vector3d(normal / norm) : Vector3d::UnitX();
    }
}

void Parametrizer::ComputeVertexArea() {
    A.resize(V.cols());
    A.setZero();

#ifdef WITH_OMP
#pragma omp parallel for
#endif
    for (int i = 0; i < V2E.size(); ++i) {
        int edge = V2E[i], stop = edge;
        if (nonManifold[i] || edge == -1) continue;
        double vertex_area = 0;
        do {
            int ep = dedge_prev_3(edge), en = dedge_next_3(edge);

            Vector3d v = V.col(F(edge % 3, edge / 3));
            Vector3d vn = V.col(F(en % 3, en / 3));
            Vector3d vp = V.col(F(ep % 3, ep / 3));

            Vector3d face_center = (v + vp + vn) * (1.0f / 3.0f);
            Vector3d prev = (v + vp) * 0.5f;
            Vector3d next = (v + vn) * 0.5f;

            vertex_area += 0.5f * ((v - prev).cross(v - face_center).norm() +
                                   (v - next).cross(v - face_center).norm());

            int opp = E2E[edge];
            if (opp == -1) break;
            edge = dedge_next_3(opp);
        } while (edge != stop);

        A[i] = vertex_area;
    }
}

#include <map>
#include <set>
#include "dset.hpp"

void Parametrizer::ComputeOrientationSingularities() {
    MatrixXd &N = hierarchy.mN[0], &Q = hierarchy.mQ[0];
    const MatrixXi& F = hierarchy.mF;
    singularities.clear();
    for (int f = 0; f < F.cols(); ++f) {
        int index = 0;
        int abs_index = 0;
        for (int k = 0; k < 3; ++k) {
            int i = F(k, f), j = F(k == 2 ? 0 : (k + 1), f);
            auto value =
                compat_orientation_extrinsic_index_4(Q.col(i), N.col(i), Q.col(j), N.col(j));
            index += value.second - value.first;
            abs_index += std::abs(value.second - value.first);
        }
        int index_mod = modulo(index, 4);
        if (index_mod == 1 || index_mod == 3) {
            if (index >= 4 || index < 0) {
                Q.col(F(0, f)) = -Q.col(F(0, f));
            }
            singularities[f] = index_mod;
        }
    }
}

void Parametrizer::ComputePositionSingularities(int with_scale) {
    const MatrixXd &V = hierarchy.mV[0], &N = hierarchy.mN[0], &Q = hierarchy.mQ[0],
                   &O = hierarchy.mO[0];
    const MatrixXi& F = hierarchy.mF;

    pos_sing.clear();
    pos_rank.resize(F.rows(), F.cols());
    pos_index.resize(6, F.cols());
    for (int f = 0; f < F.cols(); ++f) {
        if (singularities.count(f)) continue;

        Vector2i index = Vector2i::Zero();
        uint32_t i0 = F(0, f), i1 = F(1, f), i2 = F(2, f);

        Vector3d q[3] = {Q.col(i0).normalized(), Q.col(i1).normalized(), Q.col(i2).normalized()};
        Vector3d n[3] = {N.col(i0), N.col(i1), N.col(i2)};
        Vector3d o[3] = {O.col(i0), O.col(i1), O.col(i2)};
        Vector3d v[3] = {V.col(i0), V.col(i1), V.col(i2)};

        int best[3];
        double best_dp = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < 4; ++i) {
            Vector3d v0 = rotate90_by(q[0], n[0], i);
            for (int j = 0; j < 4; ++j) {
                Vector3d v1 = rotate90_by(q[1], n[1], j);
                for (int k = 0; k < 4; ++k) {
                    Vector3d v2 = rotate90_by(q[2], n[2], k);
                    double dp = std::min(std::min(v0.dot(v1), v1.dot(v2)), v2.dot(v0));
                    if (dp > best_dp) {
                        best_dp = dp;
                        best[0] = i;
                        best[1] = j;
                        best[2] = k;
                    }
                }
            }
        }
        pos_rank(0, f) = best[0];
        pos_rank(1, f) = best[1];
        pos_rank(2, f) = best[2];
        for (int k = 0; k < 3; ++k) q[k] = rotate90_by(q[k], n[k], best[k]);

        for (int k = 0; k < 3; ++k) {
            int kn = k == 2 ? 0 : (k + 1);
            double scale_x = hierarchy.mScale, scale_y = hierarchy.mScale,
                   scale_x_1 = hierarchy.mScale, scale_y_1 = hierarchy.mScale;
            if (with_scale) {
                scale_x *= hierarchy.mS[0](0, F(k, f));
                scale_y *= hierarchy.mS[0](1, F(k, f));
                scale_x_1 *= hierarchy.mS[0](0, F(kn, f));
                scale_y_1 *= hierarchy.mS[0](1, F(kn, f));
                if (best[k] % 2 != 0) std::swap(scale_x, scale_y);
                if (best[kn] % 2 != 0) std::swap(scale_x_1, scale_y_1);
            }
            double inv_scale_x = 1.0 / scale_x, inv_scale_y = 1.0 / scale_y,
                   inv_scale_x_1 = 1.0 / scale_x_1, inv_scale_y_1 = 1.0 / scale_y_1;
            std::pair<Vector2i, Vector2i> value = compat_position_extrinsic_index_4(
                v[k], n[k], q[k], o[k], v[kn], n[kn], q[kn], o[kn], scale_x, scale_y, inv_scale_x,
                inv_scale_y, scale_x_1, scale_y_1, inv_scale_x_1, inv_scale_y_1, nullptr);
            auto diff = value.first - value.second;
            index += diff;
            pos_index(k * 2, f) = diff[0];
            pos_index(k * 2 + 1, f) = diff[1];
        }

        if (index != Vector2i::Zero()) {
            pos_sing[f] = rshift90(index, best[0]);
        }
    }
}

void Parametrizer::EstimateScale() {
    auto& mF = hierarchy.mF;
    auto& mQ = hierarchy.mQ[0];
    auto& mN = hierarchy.mN[0];
    auto& mV = hierarchy.mV[0];
    FS.resize(2, mF.cols());
    FQ.resize(3, mF.cols());
#ifdef WITH_OMP
#pragma omp parallel for
#endif
    for (int i = 0; i < mF.cols(); ++i) {
        const Vector3d& n = Nf.col(i);
        const Vector3d &q_1 = mQ.col(mF(0, i)), &q_2 = mQ.col(mF(1, i)), &q_3 = mQ.col(mF(2, i));
        const Vector3d &n_1 = mN.col(mF(0, i)), &n_2 = mN.col(mF(1, i)), &n_3 = mN.col(mF(2, i));
        Vector3d q_1n = rotate_vector_into_plane(q_1, n_1, n);
        Vector3d q_2n = rotate_vector_into_plane(q_2, n_2, n);
        Vector3d q_3n = rotate_vector_into_plane(q_3, n_3, n);

        auto p = compat_orientation_extrinsic_4(q_1n, n, q_2n, n);
        Vector3d q = (p.first + p.second).normalized();
        p = compat_orientation_extrinsic_4(q, n, q_3n, n);
        q = (p.first * 2 + p.second);
        q = q - n * q.dot(n);
        FQ.col(i) = q.normalized();
    }
#ifdef WITH_OMP
#pragma omp parallel for
#endif
    for (int i = 0; i < mF.cols(); ++i) {
        double step = hierarchy.mScale * 1.f;

        const Vector3d& n = Nf.col(i);
        Vector3d p = (mV.col(mF(0, i)) + mV.col(mF(1, i)) + mV.col(mF(2, i))) * (1.0 / 3.0);
        Vector3d q_x = FQ.col(i), q_y = n.cross(q_x);
        Vector3d q_xl = -q_x, q_xr = q_x;
        Vector3d q_yl = -q_y, q_yr = q_y;
        Vector3d q_yl_unfold = q_y, q_yr_unfold = q_y, q_xl_unfold = q_x, q_xr_unfold = q_x;
        int f;
        double tx, ty, len;

        f = i;
        len = step;
        TravelField(p, q_xl, len, f, hierarchy.mE2E, mV, mF, Nf, FQ, mQ, mN, triangle_space, &tx,
                    &ty, &q_yl_unfold);

        f = i;
        len = step;
        TravelField(p, q_xr, len, f, hierarchy.mE2E, mV, mF, Nf, FQ, mQ, mN, triangle_space, &tx,
                    &ty, &q_yr_unfold);

        f = i;
        len = step;
        TravelField(p, q_yl, len, f, hierarchy.mE2E, mV, mF, Nf, FQ, mQ, mN, triangle_space, &tx,
                    &ty, &q_xl_unfold);

        f = i;
        len = step;
        TravelField(p, q_yr, len, f, hierarchy.mE2E, mV, mF, Nf, FQ, mQ, mN, triangle_space, &tx,
                    &ty, &q_xr_unfold);
        double dSx = (q_yr_unfold - q_yl_unfold).dot(q_x) / (2.0f * step);
        double dSy = (q_xr_unfold - q_xl_unfold).dot(q_y) / (2.0f * step);
        FS.col(i) = Vector2d(dSx, dSy);
    }

    std::vector<double> areas(mV.cols(), 0.0);
    for (int i = 0; i < mF.cols(); ++i) {
        Vector3d p1 = mV.col(mF(1, i)) - mV.col(mF(0, i));
        Vector3d p2 = mV.col(mF(2, i)) - mV.col(mF(0, i));
        double area = p1.cross(p2).norm();
        for (int j = 0; j < 3; ++j) {
            auto index = compat_orientation_extrinsic_index_4(FQ.col(i), Nf.col(i),
                                                              mQ.col(mF(j, i)), mN.col(mF(j, i)));
            double scaleX = FS.col(i).x(), scaleY = FS.col(i).y();
            if (index.first != index.second % 2) {
                std::swap(scaleX, scaleY);
            }
            if (index.second >= 2) {
                scaleX = -scaleX;
                scaleY = -scaleY;
            }
            {
                hierarchy.mK[0].col(mF(j, i)) += area * Vector2d(scaleX, scaleY);
                areas[mF(j, i)] += area;
            }
        }
    }
#ifdef WITH_OMP
#pragma omp parallel for
#endif
    for (int i = 0; i < mV.cols(); ++i) {
        if (areas[i] != 0) hierarchy.mK[0].col(i) /= areas[i];
    }
    for (int l = 0; l < hierarchy.mK.size() - 1; ++l) {
        const MatrixXd& K = hierarchy.mK[l];
        MatrixXd& K_next = hierarchy.mK[l + 1];
        auto& toUpper = hierarchy.mToUpper[l];
#ifdef WITH_OMP
#pragma omp parallel for
#endif
        for (int i = 0; i < toUpper.cols(); ++i) {
            Vector2i upper = toUpper.col(i);
            Vector2d k0 = K.col(upper[0]);

            if (upper[1] != -1) {
                Vector2d k1 = K.col(upper[1]);
                k0 = 0.5 * (k0 + k1);
            }

            K_next.col(i) = k0;
        }
    }
}

void Parametrizer::SaveToFile(FILE* fp) {
    Save(fp, singularities);
    Save(fp, pos_sing);
    Save(fp, pos_rank);
    Save(fp, pos_index);

    // input mesh
    Save(fp, V);
    Save(fp, N);
    Save(fp, Nf);
    Save(fp, FS);
    Save(fp, FQ);
    Save(fp, F);
    Save(fp, triangle_space);

    // data structures
    Save(fp, V2E);
    Save(fp, E2E);
    Save(fp, boundary);
    Save(fp, nonManifold);
    Save(fp, adj);
    hierarchy.SaveToFile(fp);

    // Mesh Status;
    Save(fp, surface_area);
    Save(fp, scale);
    Save(fp, average_edge_length);
    Save(fp, max_edge_length);
    Save(fp, A);

    // target mesh
    Save(fp, num_vertices);
    Save(fp, num_faces);

    // just for test
    //	Save(fp, colors);
    //	Save(fp, compact_num_v);
    //	Save(fp, O_compact);
    //	Save(fp, counter);
    //	Save(fp, edge_diff);
    //	Save(fp, edge_ids);
    //	Save(fp, edge_values);

    //	Save(fp, constraints_index);
    //	Save(fp, constraints_sign);
    //	Save(fp, variables);
    //	Save(fp, parentss);
    //	Save(fp, edge_to_constraints);

    //	Save(fp, param);
    //	Save(fp, min_param);
    //	Save(fp, max_param);
    //	Save(fp, cuts);
}

void Parametrizer::LoadFromFile(FILE* fp) {
    Read(fp, singularities);
    Read(fp, pos_sing);
    Read(fp, pos_rank);
    Read(fp, pos_index);

    // input mesh
    Read(fp, V);
    Read(fp, N);
    Read(fp, Nf);
    Read(fp, FS);
    Read(fp, FQ);
    Read(fp, F);
    Read(fp, triangle_space);

    // data structures
    Read(fp, V2E);
    Read(fp, E2E);
    Read(fp, boundary);
    Read(fp, nonManifold);
    Read(fp, adj);
    hierarchy.LoadFromFile(fp);

    // Mesh Status;
    Read(fp, surface_area);
    Read(fp, scale);
    Read(fp, average_edge_length);
    Read(fp, max_edge_length);
    Read(fp, A);

    // target mesh
    Read(fp, num_vertices);
    Read(fp, num_faces);

    // just for test
    //	Read(fp, colors);
    //	Read(fp, compact_num_v);
    //	Read(fp, O_compact);
    //	Read(fp, counter);
    //	Read(fp, edge_diff);
    //	Read(fp, edge_ids);
    //	Read(fp, edge_values);

    //	Read(fp, constraints_index);
    //	Read(fp, constraints_sign);
    //	Read(fp, variables);
    //	Read(fp, parentss);
    //	Read(fp, edge_to_constraints);

    //	Read(fp, param);
    //	Read(fp, min_param);
    //	Read(fp, max_param);
    //	Read(fp, cuts);
}

int get_parents(std::vector<std::pair<int, int>>& parents, int j) {
    if (j == parents[j].first) return j;
    int k = get_parents(parents, parents[j].first);
    parents[j].second = (parents[j].second + parents[parents[j].first].second) % 4;
    parents[j].first = k;
    return k;
}

int get_parents_orient(std::vector<std::pair<int, int>>& parents, int j) {
    if (j == parents[j].first) return parents[j].second;
    return (parents[j].second + get_parents_orient(parents, parents[j].first)) % 4;
}

void Parametrizer::BuildEdgeInfo() {
    auto& F = hierarchy.mF;
    auto& E2E = hierarchy.mE2E;

    edge_diff.clear();
    edge_values.clear();
    face_edgeIds.resize(F.cols(), Vector3i(-1, -1, -1));
    for (int i = 0; i < F.cols(); ++i) {
        for (int j = 0; j < 3; ++j) {
            //			if (face_edgeIds[i][j] != -1)
            //				continue;
            int k1 = j, k2 = (j + 1) % 3;
            int v1 = F(k1, i);
            int v2 = F(k2, i);
            DEdge e2(v1, v2);
            Vector2i diff2;
            int rank2;
            if (v1 > v2) {
                rank2 = pos_rank(k2, i);
                diff2 =
                    rshift90(Vector2i(-pos_index(k1 * 2, i), -pos_index(k1 * 2 + 1, i)), rank2);
            } else {
                rank2 = pos_rank(k1, i);
                diff2 = rshift90(Vector2i(pos_index(k1 * 2, i), pos_index(k1 * 2 + 1, i)), rank2);
            }
            int current_eid = i * 3 + k1;
            int eid = E2E[current_eid];
            int eID2 = face_edgeIds[eid / 3][eid % 3];
            if (eID2 == -1) {
                eID2 = edge_values.size();
                edge_values.push_back(e2);
                edge_diff.push_back(diff2);
                face_edgeIds[i][k1] = eID2;
                if (eid != -1) face_edgeIds[eid / 3][eid % 3] = eID2;
            } else if (!singularities.count(i)) {
                edge_diff[eID2] = diff2;
            }
        }
    }
}

void Parametrizer::ComputePosition(int with_scale) {
    auto& V = hierarchy.mV[0];
    auto& F = hierarchy.mF;
    auto& Q = hierarchy.mQ[0];
    auto& N = hierarchy.mN[0];
    auto& O = hierarchy.mO[0];
    //	auto &S = hierarchy.mS[0];
    int t1 = GetCurrentTime64();
    std::vector<std::unordered_map<int, double>> entries(V.cols() * 2);
    std::vector<double> b(V.cols() * 2);
    for (int e = 0; e < edge_diff.size(); ++e) {
        int v1 = edge_values[e].x;
        int v2 = edge_values[e].y;
        Vector3d q_1 = Q.col(v1);
        Vector3d q_2 = Q.col(v2);
        Vector3d n_1 = N.col(v1);
        Vector3d n_2 = N.col(v2);
        Vector3d q_1_y = n_1.cross(q_1);
        Vector3d q_2_y = n_2.cross(q_2);
        Vector3d weights[] = {q_2, q_2_y, -q_1, -q_1_y};
        auto index = compat_orientation_extrinsic_index_4(q_1, n_1, q_2, n_2);
        //		double s_x1 = S(0, v1), s_y1 = S(1, v1);
        //		double s_x2 = S(0, v2), s_y2 = S(1, v2);
        int rank_diff = (index.second + 4 - index.first) % 4;
        Vector3d qd_x = 0.5 * (rotate90_by(q_2, n_2, rank_diff) + q_1);
        Vector3d qd_y = 0.5 * (rotate90_by(q_2_y, n_2, rank_diff) + q_1_y);
        double scale_x = /*(with_scale ? 0.5 * (s_x1 + s_x2) : 1) */ hierarchy.mScale;
        double scale_y = /*(with_scale ? 0.5 * (s_y1 + s_y2) : 1) */ hierarchy.mScale;
        Vector2i diff = edge_diff[e];
        Vector3d C = diff[0] * scale_x * qd_x + diff[1] * scale_y * qd_y + V.col(v1) - V.col(v2);
        int vid[] = {v2 * 2, v2 * 2 + 1, v1 * 2, v1 * 2 + 1};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                auto it = entries[vid[i]].find(vid[j]);
                if (it == entries[vid[i]].end()) {
                    entries[vid[i]][vid[j]] = weights[i].dot(weights[j]);
                } else {
                    entries[vid[i]][vid[j]] += weights[i].dot(weights[j]);
                }
            }
            b[vid[i]] += weights[i].dot(C);
        }
    }

    std::vector<double> D(V.cols() * 2);
    for (int i = 0; i < D.size(); ++i) {
        D[i] = 1.0 / entries[i][i];
    }
    std::vector<double> x(V.cols() * 2);
    std::vector<double> R;
    std::vector<int> R_ind;
    std::vector<int> R_offset(V.cols() * 2 + 1);
#ifdef WITH_OMP
#pragma omp parallel for
#endif
    for (int i = 0; i < O.cols(); ++i) {
        Vector3d q = Q.col(i);
        Vector3d n = N.col(i);
        Vector3d q_y = n.cross(q);
        x[i * 2] = (O.col(i) - V.col(i)).dot(q);
        x[i * 2 + 1] = (O.col(i) - V.col(i)).dot(q_y);
    }
    for (int i = 0; i < entries.size(); ++i) {
        R_offset[i] = R.size();
        for (auto& p : entries[i]) {
            if (p.first == i) continue;
            R_ind.push_back(p.first);
            R.push_back(p.second);
        }
    }
    R_offset.back() = R.size();
#ifdef WITH_CUDA
    JacobiSolve(D, R, R_ind, R_offset, x, b);
#else
#ifndef WITH_JACOBI
    std::vector<Eigen::Triplet<double>> lhsTriplets;
    lhsTriplets.reserve(F.cols() * 6);
    Eigen::SparseMatrix<double> A(V.cols() * 2, V.cols() * 2);
    VectorXd rhs(V.cols() * 2);
    rhs.setZero();
    for (int i = 0; i < entries.size(); ++i) {
        rhs(i) = b[i];
        for (auto& rec : entries[i]) {
            lhsTriplets.push_back(Eigen::Triplet<double>(i, rec.first, rec.second));
        }
    }

    A.setFromTriplets(lhsTriplets.begin(), lhsTriplets.end());
    Eigen::SimplicialLLT<Eigen::SparseMatrix<double>> solver;
    solver.analyzePattern(A);
    solver.factorize(A);
    VectorXd x_new = solver.solve(rhs);
    for (int i = 0; i < x.size(); ++i) x[i] = x_new[i];
#else
    std::vector<double> x_new(x.size(), 0);
    for (int iter = 0; iter < 30; ++iter) {
        double err = 0;
#ifdef WITH_OMP
#pragma omp parallel for
#endif
        for (int i = 0; i < x.size(); ++i) {
            x_new[i] = b[i];
            for (int j = R_offset[i]; j < R_offset[i + 1]; ++j) {
                x_new[i] -= R[j] * x[R_ind[j]];
            }
            x_new[i] *= D[i];
            err += (x_new[i] - x[i]) * (x_new[i] - x[i]);
        }
        std::swap(x_new, x);
    }
    for (int i = 0; i < x.size(); ++i) {
        if (abs(x[i] - x1[i]) > 1e-6) {
            printf("fail %lf %lf\n", x[i], x1[i]);
        }
    }
    printf("finish...\n");
    exit(0);
#endif
#endif
    for (int i = 0; i < O.cols(); ++i) {
        Vector3d q = Q.col(i);
        Vector3d n = N.col(i);
        Vector3d q_y = n.cross(q);
        O.col(i) = V.col(i) + q * x[i * 2] + q_y * x[i * 2 + 1];
    }

    int t2 = GetCurrentTime64();
    printf("Use %lf seconds.\n", (t2 - t1) * 1e-3);
}

void Parametrizer::ComputeIndexMap(int with_scale) {
    // build edge info
    auto& V = hierarchy.mV[0];
    auto& F = hierarchy.mF;
    auto& Q = hierarchy.mQ[0];
    auto& N = hierarchy.mN[0];
    auto& O = hierarchy.mO[0];
    ComputeOrientationSingularities();

    BuildEdgeInfo();

    for (int i = 0; i < edge_diff.size(); ++i) {
        for (int j = 0; j < 2; ++j) {
            if (abs(edge_diff[i][j]) > 1) {
                edge_diff[i][j] /= abs(edge_diff[i][j]);
            }
        }
    }
#ifdef LOG_OUTPUT
    printf("Build Integer Constraints...\n");
#endif
    BuildIntegerConstraints();

    ComputeMaxFlow();

#ifdef LOG_OUTPUT
    printf("Fix flip advance...\n");
#endif
    subdivide_diff(F, V, N, Q, O, V2E, hierarchy.mE2E, boundary, nonManifold, edge_diff,
                   edge_values, face_edgeOrients, face_edgeIds, singularities);

    int t1 = GetCurrentTime64();
    FixFlipAdvance();
    int t2 = GetCurrentTime64();
    printf("Flip use %lf\n", (t2 - t1) * 1e-3);
//    subdivide_diff(F, V, N, Q, O, V2E, hierarchy.mE2E, boundary, nonManifold, edge_diff,
//                   edge_values, face_edgeOrients, face_edgeIds, singularities);

    disajoint_tree = DisajointTree(V.cols());
    for (int i = 0; i < edge_diff.size(); ++i) {
        if (edge_diff[i] == Vector2i::Zero()) {
            int vv0 = edge_values[i].x;
            int vv1 = edge_values[i].y;
            disajoint_tree.Merge(vv0, vv1);
        }
    }
    disajoint_tree.BuildCompactParent();

    ComputePosition(with_scale);
    int num_v = disajoint_tree.CompactNum();
    O_compact.resize(num_v, Vector3d::Zero());
    Q_compact.resize(num_v, Vector3d::Zero());
    N_compact.resize(num_v, Vector3d::Zero());
    counter.resize(num_v, 0);
    for (int i = 0; i < O.cols(); ++i) {
        int compact_v = disajoint_tree.Index(i);
        O_compact[compact_v] += O.col(i);
        N_compact[compact_v] = N_compact[compact_v] * counter[compact_v] + N.col(i);
        N_compact[compact_v].normalize();
        if (counter[compact_v] == 0)
            Q_compact[compact_v] = Q.col(i);
        else {
            auto pairs = compat_orientation_extrinsic_4(Q_compact[compact_v], N_compact[compact_v],
                                                        Q.col(i), N.col(i));
            Q_compact[compact_v] = (pairs.first * counter[compact_v] + pairs.second).normalized();
        }
        counter[compact_v] += 1;
    }
    for (int i = 0; i < O_compact.size(); ++i) {
        O_compact[i] /= counter[i];
    }

#ifdef LOG_OUTPUT
    printf("extract graph...\n");
#endif
    std::vector<std::set<int>> vertices(num_v), complete_set(num_v);
    for (int i = 0; i < edge_diff.size(); ++i) {
        int p1 = disajoint_tree.Index(edge_values[i].x);
        int p2 = disajoint_tree.Index(edge_values[i].y);
        if (p1 == p2) continue;
        complete_set[p1].insert(p2);
        complete_set[p2].insert(p1);
        if (abs(edge_diff[i][0]) + abs(edge_diff[i][1]) == 1) {
            vertices[p1].insert(p2);
            vertices[p2].insert(p1);
        }
    }

#ifdef LOG_OUTPUT
    printf("extract bad vertices...\n");
#endif
    bad_vertices.resize(num_v, 0);
    std::queue<int> badq;
    for (int i = 0; i < num_v; ++i) {
        if (vertices[i].size() < 3) {
            badq.push(i);
            bad_vertices[i] = 1;
        }
    }
    while (!badq.empty()) {
        int v = badq.front();
        badq.pop();
        for (auto& v1 : vertices[v]) {
            vertices[v1].erase(v);
            if (vertices[v1].size() < 3 && bad_vertices[v1] == 0) {
                bad_vertices[v1] = 1;
                badq.push(v1);
            }
        }
    }
    std::set<DEdge> bad_edges;
    for (int i = 0; i < F.cols(); ++i) {
        int v0 = F(0, i), p0 = disajoint_tree.Index(v0);
        int v1 = F(1, i), p1 = disajoint_tree.Index(v1);
        int v2 = F(2, i), p2 = disajoint_tree.Index(v2);
        if (p0 == p1 || p1 == p2 || p2 == p0) continue;
        Vector2i diff[3];
        for (int j = 0; j < 3; ++j) {
            int eid = face_edgeIds[i][j];
            diff[j] = rshift90(edge_diff[eid], face_edgeOrients[i][j]);
        }
        int a = -diff[0][0] * diff[2][1] + diff[0][1] * diff[2][0];
        if (a < 0) {
            for (int j = 0; j < 3; ++j) {
                int t1 = disajoint_tree.Index(F(j, i));
                int t2 = disajoint_tree.Index(F((j + 1) % 3, i));
                if (t1 != t2) bad_edges.insert(DEdge(t1, t2));
            }
        }
    }
#ifdef LOG_OUTPUT
    printf("extract quad cells...\n");
#endif
    std::map<DEdge, std::pair<Vector3i, Vector3i>> quad_cells;
    for (int i = 0; i < F.cols(); ++i) {
        int v0 = F(0, i), p0 = disajoint_tree.Index(v0);
        int v1 = F(1, i), p1 = disajoint_tree.Index(v1);
        int v2 = F(2, i), p2 = disajoint_tree.Index(v2);
        if (p0 != p1 && p1 != p2 && p2 != p0 && bad_vertices[p0] == 0 && bad_vertices[p1] == 0 &&
            bad_vertices[p2] == 0 && bad_edges.count(DEdge(p0, p1)) == 0 &&
            bad_edges.count(DEdge(p1, p2)) == 0 && bad_edges.count(DEdge(p2, p0)) == 0) {
            auto diff1 = edge_diff[face_edgeIds[i][0]];
            auto diff2 = edge_diff[face_edgeIds[i][1]];
            auto diff3 = edge_diff[face_edgeIds[i][2]];
            int orient1 = face_edgeOrients[i][0];
            int orient2 = face_edgeOrients[i][2];
            auto d1 = rshift90(diff1, orient1);
            auto d2 = rshift90(-diff3, orient2);
            if (d1[0] * d2[1] - d1[1] * d2[0] < 0) continue;
            DEdge eid;
            if (abs(diff1[0]) == 1 && abs(diff1[1]) == 1) {
                eid = DEdge(p0, p1);
            } else if (abs(diff2[0]) == 1 && abs(diff2[1]) == 1) {
                int t = p0;
                p0 = p1;
                p1 = p2;
                p2 = t;
                eid = DEdge(p0, p1);
            } else if (abs(diff3[0]) == 1 && abs(diff3[1]) == 1) {
                int t = p1;
                p1 = p0;
                p0 = p2;
                p2 = t;
                eid = DEdge(p0, p1);
            } else {
                continue;
            }
            if (quad_cells.count(eid) == 0)
                quad_cells[eid] = std::make_pair(Vector3i(p0, p1, p2), Vector3i(-100, -100, -100));
            else
                quad_cells[eid].second = Vector3i(p0, p1, p2);
        }
    }
#ifdef LOG_OUTPUT
    printf("extract quads...\n");
#endif
    for (auto& c : quad_cells) {
        if (c.second.second != Vector3i(-100, -100, -100)) {
            F_compact.push_back(Vector4i(c.second.first[0], c.second.second[2], c.second.first[1],
                                         c.second.first[2]));
        }
    }
#ifdef LOG_OUTPUT
    printf("Fix holes...\n");
#endif
    FixHoles();

    // potential bug, not guarantee to have quads at holes!
#ifdef LOG_OUTPUT
    printf("Direct Quad Graph...\n");
#endif
    compute_direct_graph_quad(O_compact, F_compact, V2E_compact, E2E_compact, boundary_compact,
                              nonManifold_compact);
#ifdef LOG_OUTPUT
    printf("Optimize quad positions...\n");
#endif

//    optimize_quad_positions(O_compact, N_compact, Q_compact, F_compact, V2E_compact, E2E_compact,
//                            V, N, Q, O, F, V2E, hierarchy.mE2E, disajoint_tree, hierarchy.mScale);

}

void Parametrizer::FixHoles() {
    std::unordered_map<long long, std::pair<int, int>> edge_to_faces;
    std::unordered_set<long long> directed_edges;
    int numV = disajoint_tree.CompactNum();
    for (int i = 0; i < F_compact.size(); ++i) {
        for (int j = 0; j < 4; ++j) {
            int v1 = F_compact[i][j];
            int v2 = F_compact[i][(j + 1) % 4];
            DEdge e(v1, v2);
            long long hash = (long long)numV * e.x + e.y;
            directed_edges.insert((long long)numV * v1 + v2);
            auto it = edge_to_faces.find(hash);
            if (v1 < v2) {
                if (it == edge_to_faces.end()) {
                    edge_to_faces[hash] = std::make_pair(i * 4 + j, -1);
                } else {
                    edge_to_faces[hash].first = i * 4 + j;
                }
            } else {
                if (it == edge_to_faces.end()) {
                    edge_to_faces[hash] = std::make_pair(-1, i * 4 + j);
                } else {
                    edge_to_faces[hash].second = i * 4 + j;
                }
            }
        }
    }
    int boundary = 0;
    std::unordered_map<long long, int> boundary_edge_id;
    std::vector<DEdge> boundary_edges;
    for (auto& p : edge_to_faces) {
        if (p.second.first == -1 || p.second.second == -1) {
            if (boundary_edge_id.count(p.first) == 0) {
                boundary_edges.push_back(
                    DEdge(p.first / numV, p.first % disajoint_tree.CompactNum()));
                boundary_edge_id[p.first] = boundary++;
            }
        }
    }
    std::vector<std::list<int>> graph(boundary_edges.size());
    for (int i = 0; i < boundary_edges.size(); ++i) {
        for (int j = 0; j < boundary_edges.size(); ++j) {
            if (i == j) continue;
            auto& e1 = boundary_edges[i];
            auto& e2 = boundary_edges[j];
            if (e1.x == e2.x || e1.y == e2.x || e1.x == e2.y || e1.y == e2.y) {
                graph[i].push_back(j);
                graph[j].push_back(i);
            }
        }
    }
    std::vector<int> visited(graph.size(), -1);
    int loop_id = 0;
    for (int i = 0; i < graph.size(); ++i) {
        if (visited[i] != -1) continue;
        std::vector<int> loop_edge;
        loop_edge.push_back(i);
        visited[i] = loop_id;
        while (true) {
            bool update = false;
            int vert = loop_edge.back();
            for (auto& next_vert : graph[vert]) {
                if (visited[next_vert] == -1) {
                    update = true;
                    visited[next_vert] = loop_id;
                    loop_edge.push_back(next_vert);
                    break;
                }
            }
            if (!update) break;
        }
        if (loop_edge.size() < 2) {
            printf("irregular %d\n", (int)loop_edge.size());
            continue;
        }
        std::vector<int> loop_vertices;
        for (int i = 0; i < loop_edge.size(); ++i) {
            int e1 = loop_edge[i];
            int e2 = loop_edge[(i + 1) % loop_edge.size()];
            int v1 = boundary_edges[e1].x;
            if (v1 == boundary_edges[e2].x || v1 == boundary_edges[e2].y)
                v1 = boundary_edges[e1].y;
            loop_vertices.push_back(v1);
        }
        while (!loop_vertices.empty()) {
            if (loop_vertices.size() <= 4) {
                if (loop_vertices.size() == 4)
                    F_compact.push_back(Vector4i(loop_vertices[0], loop_vertices[1],
                                                 loop_vertices[2], loop_vertices[3]));
                else
                    F_compact.push_back(Vector4i(loop_vertices[0], loop_vertices[1],
                                                 loop_vertices[2], loop_vertices[2]));

                if (directed_edges.count((long long)loop_vertices[0] * numV + loop_vertices[1])) {
                    std::swap(F_compact.back()[1], F_compact.back()[3]);
                }
                loop_edge.clear();
                break;
            }
            double min_dis = 1e30;
            int v_start = -1;
            for (int i = 0; i < loop_edge.size(); ++i) {
                int v1 = loop_vertices[i];
                int v2 = loop_vertices[(i + 3) % loop_edge.size()];
                double dis = (O_compact[v1] - O_compact[v2]).norm();
                if (dis < min_dis) {
                    min_dis = dis;
                    v_start = i;
                }
            }
            F_compact.push_back(Vector4i(loop_vertices[v_start],
                                         loop_vertices[(v_start + 1) % loop_vertices.size()],
                                         loop_vertices[(v_start + 2) % loop_vertices.size()],
                                         loop_vertices[(v_start + 3) % loop_vertices.size()]));
            if (directed_edges.count((long long)F_compact.back()[0] * numV +
                                     F_compact.back()[1])) {
                std::swap(F_compact.back()[1], F_compact.back()[3]);
            }
            int delete_v1 = (v_start + 1) % loop_vertices.size();
            int delete_v2 = (v_start + 2) % loop_vertices.size();
            if (delete_v1 > delete_v2) std::swap(delete_v1, delete_v2);
            loop_vertices.erase(loop_vertices.begin() + delete_v2);
            loop_vertices.erase(loop_vertices.begin() + delete_v1);
        }
        loop_id += 1;
    }
}

void Parametrizer::BuildIntegerConstraints() {
    auto& F = hierarchy.mF;
    auto& Q = hierarchy.mQ[0];
    auto& N = hierarchy.mN[0];
    std::vector<Vector2i> sign_indices;
    face_edgeOrients.resize(F.cols());
    std::vector<Vector4i> edge_to_constraints;
    edge_to_constraints.resize(edge_values.size(), Vector4i(-1, -1, -1, -1));

    for (int i = 0; i < F.cols(); ++i) {
        int v0 = F(0, i);
        int v1 = F(1, i);
        int v2 = F(2, i);
        DEdge e0(v0, v1), e1(v1, v2), e2(v2, v0);
        const Vector3i& eid = face_edgeIds[i];
        Vector2i vid[3];
        for (int i = 0; i < 3; ++i) {
            vid[i] = Vector2i(eid[i] * 2 + 1, eid[i] * 2 + 2);
        }
        auto index1 =
            compat_orientation_extrinsic_index_4(Q.col(v0), N.col(v0), Q.col(v1), N.col(v1));
        auto index2 =
            compat_orientation_extrinsic_index_4(Q.col(v0), N.col(v0), Q.col(v2), N.col(v2));
        int rank1 = (index1.first - index1.second + 4) % 4;
        int rank2 = (index2.first - index2.second + 4) % 4;
        int orients[3] = {0};
        if (v1 < v0) {
            vid[0] = -rshift90(vid[0], rank1);
            orients[0] = (rank1 + 2) % 4;
        }
        if (v2 < v1) {
            vid[1] = -rshift90(vid[1], rank2);
            orients[1] = (rank2 + 2) % 4;
        } else {
            vid[1] = rshift90(vid[1], rank1);
            orients[1] = rank1;
        }
        if (v2 < v0) {
            vid[2] = rshift90(vid[2], rank2);
            orients[2] = rank2;
        } else {
            vid[2] = -vid[2];
            orients[2] = 2;
        }
        face_edgeOrients[i] = Vector3i(orients[0], orients[1], orients[2]);
        edge_to_constraints[eid[0]][(v0 > v1) * 2] = i;
        edge_to_constraints[eid[0]][(v0 > v1) * 2 + 1] = orients[0];
        edge_to_constraints[eid[1]][(v1 > v2) * 2] = i;
        edge_to_constraints[eid[1]][(v1 > v2) * 2 + 1] = orients[1];
        edge_to_constraints[eid[2]][(v2 > v0) * 2] = i;
        edge_to_constraints[eid[2]][(v2 > v0) * 2 + 1] = orients[2];

        for (int k = 0; k < 3; ++k) {
            sign_indices.push_back(vid[k]);
        }
    }

    DisajointOrientTree disajoint_orient_tree = DisajointOrientTree(F.cols());
    for (int i = 0; i < edge_to_constraints.size(); ++i) {
        auto& edge_c = edge_to_constraints[i];
        int v0 = edge_c[0];
        int v1 = edge_c[2];
        if (singularities.count(v0) || singularities.count(v1)) continue;
        int orient1 = edge_c[1];
        int orient0 = (edge_c[3] + 2) % 4;
        disajoint_orient_tree.Merge(v0, v1, orient0, orient1);
    }

    std::vector<Vector3i> sing_diff;
    std::vector<std::unordered_map<int, std::pair<int, int>>> sing_maps;
    std::vector<Vector3i> sing_orients;
    for (int i = 0; i < sign_indices.size(); i += 3) {
        int f = i / 3;
        int orient = disajoint_orient_tree.Orient(f);
        for (int j = 0; j < 3; ++j) {
            sign_indices[i + j] = rshift90(sign_indices[i + j], orient);
        }
        for (int j = 0; j < 2; ++j) {
            Vector3i sign, ind;
            for (int k = 0; k < 3; ++k) {
                ind[k] = abs(sign_indices[i + k][j]);
                if (ind[k] == 0) {
                    printf("OMG!\n");
                    exit(0);
                }
                sign[k] = sign_indices[i + k][j] / ind[k];
                ind[k] -= 1;
            }
            constraints_index.push_back(ind);
            constraints_sign.push_back(sign);
        }
        if (singularities.count(f)) {
            int orient_base = singularities[f];
            Vector3i diffs;
            Vector3i orient_diffs;
            for (int j = 0; j < 3; ++j) {
                int eid = face_edgeIds[f][(j + 1) % 3];
                int v0 = edge_to_constraints[eid][0];
                int v1 = edge_to_constraints[eid][2];
                int orientp0 = disajoint_orient_tree.Orient(v0) + edge_to_constraints[eid][1];
                int orientp1 = disajoint_orient_tree.Orient(v1) + edge_to_constraints[eid][3];
                int orient_diff = 0;
                if (v1 == f)
                    orient_diff = (orientp0 - orientp1 + 6) % 4;
                else
                    orient_diff = (orientp1 - orientp0 + 6) % 4;
                Vector2i sign_index[3];
                sign_index[0] = rshift90(sign_indices[i + j], (orient_base + orient_diff) % 4);
                sign_index[1] = rshift90(sign_indices[i + (j + 1) % 3], orient_diff);
                sign_index[2] = rshift90(sign_indices[i + (j + 2) % 3], orient_diff);
                int total_diff = 0;
                for (int k = 0; k < 2; ++k) {
                    auto ind = constraints_index[f * 2 + k];
                    auto sign = constraints_sign[f * 2 + k];
                    for (int l = 0; l < 3; ++l) {
                        ind[l] = abs(sign_index[l][k]);
                        sign[l] = sign_index[l][k] / ind[l];
                        ind[l] -= 1;
                    }
                    int diff1 = edge_diff[ind[0] / 2][ind[0] % 2];
                    int diff2 = edge_diff[ind[1] / 2][ind[1] % 2];
                    int diff3 = edge_diff[ind[2] / 2][ind[2] % 2];
                    int diff = sign[0] * diff1 + sign[1] * diff2 + sign[2] * diff3;
                    total_diff += diff;
                }
                orient_diffs[j] = orient_diff;
                diffs[j] = total_diff;
            }
            sing_diff.push_back(diffs);
            sing_orients.push_back(orient_diffs);
        }
    }

    int total_flow = 0;
    for (int i = 0; i < constraints_index.size(); ++i) {
        if (singularities.count(i / 2)) {
            continue;
        }
        auto index = constraints_index[i];
        auto sign = constraints_sign[i];
        int diff1 = edge_diff[index[0] / 2][index[0] % 2];
        int diff2 = edge_diff[index[1] / 2][index[1] % 2];
        int diff3 = edge_diff[index[2] / 2][index[2] % 2];
        int diff = sign[0] * diff1 + sign[1] * diff2 + sign[2] * diff3;
        total_flow += diff;
    }

    sing_maps.resize(sing_diff.size() + 1);
    sing_maps[0][total_flow] = std::make_pair(0, 0);
    for (int i = 0; i < sing_diff.size(); ++i) {
        auto& prev = sing_maps[i];
        auto& next = sing_maps[i + 1];
        for (auto& p : prev) {
            for (int j = 0; j < 3; ++j) {
                int v = p.first + sing_diff[i][j];
                int t = p.second.first + abs(sing_diff[i][j]);
                auto it = next.find(v);
                if (it == next.end())
                    next[v] = std::make_pair(t, j);
                else if (t < it->second.first)
                    it->second = std::make_pair(t, j);
            }
        }
    }

    std::vector<int> sing_selection;
    int target_flow = 0;
    while (sing_maps.back().count(target_flow) == 0 && sing_maps.back().count(-target_flow) == 0) {
        target_flow += 2;
    }
    if (sing_maps.back().count(target_flow) == 0) target_flow = -target_flow;
    int remain_flow = target_flow;
    for (int i = sing_diff.size(); i > 0; i--) {
        auto p = sing_maps[i][remain_flow];
        remain_flow -= sing_diff[i - 1][p.second];
        sing_selection.push_back(p.second);
    }
    std::reverse(sing_selection.begin(), sing_selection.end());
    int sing_count = 0;
    for (auto& f : singularities) {
        int select = sing_selection[sing_count];
        int orient_diff = sing_orients[sing_count++][select];
        auto& index1 = constraints_index[f.first * 2];
        auto& index2 = constraints_index[f.first * 2 + 1];
        auto& sign1 = constraints_sign[f.first * 2];
        auto& sign2 = constraints_sign[f.first * 2 + 1];

        int eid0;
        for (int i = 0; i < 3; ++i) {
            auto diff = Vector2i(sign1[i] * (index1[i] + 1), sign2[i] * (index2[i] + 1));
            int t = orient_diff;
            if (i == select) t = (t + f.second) % 4;
            int v0 = F(i, f.first);
            int v1 = F((i + 1) % 3, f.first);
            int eid = face_edgeIds[f.first][i];
            if ((select + 1) % 3 == i) eid0 = eid;
            edge_to_constraints[eid][(v0 > v1) * 2] = f.first;
            edge_to_constraints[eid][(v0 > v1) * 2 + 1] =
                (edge_to_constraints[eid][(v0 > v1) * 2 + 1] + t) % 4;
            face_edgeOrients[f.first][i] = (face_edgeOrients[f.first][i] + t) % 4;

            diff = rshift90(diff, t);
            index1[i] = abs(diff[0]);
            sign1[i] = diff[0] / abs(diff[0]);
            index1[i] -= 1;
            index2[i] = abs(diff[1]);
            sign2[i] = diff[1] / abs(diff[1]);
            index2[i] -= 1;
        }
        auto& edge_c = edge_to_constraints[eid0];
        int v0 = edge_c[0];
        int v1 = edge_c[2];

        int orient1 = edge_c[1];
        int orient0 = (edge_c[3] + 2) % 4;

        disajoint_orient_tree.Merge(v0, v1, orient0, orient1);
    }
    total_flow = 0;

    variables.resize(edge_diff.size() * 2, std::make_pair(Vector2i(-1, -1), 0));
    for (int i = 0; i < constraints_index.size(); ++i) {
        auto index = constraints_index[i];
        auto sign = constraints_sign[i];
        int diff1 = edge_diff[index[0] / 2][index[0] % 2];
        int diff2 = edge_diff[index[1] / 2][index[1] % 2];
        int diff3 = edge_diff[index[2] / 2][index[2] % 2];
        int diff = sign[0] * diff1 + sign[1] * diff2 + sign[2] * diff3;
        total_flow += diff;
        for (int j = 0; j < 3; ++j) {
            auto& p = variables[index[j]].first;
            if (sign[j] > 0)
                p[0] = i;
            else
                p[1] = i;
            variables[index[j]].second += sign[j];
        }
    }
    cuts.clear();
    std::vector<std::pair<int, int>> modified_variables;
    for (int i = 0; i < variables.size(); ++i) {
        if (variables[i].second != 0) {
            cuts.insert(edge_values[i / 2]);
            if (target_flow > 0) {
                if (variables[i].second > 0 && edge_diff[i / 2][i % 2] > -1) {
                    modified_variables.push_back(std::make_pair(i, -1));
                }
                if (variables[i].second < 0 && edge_diff[i / 2][i % 2] < 1) {
                    modified_variables.push_back(std::make_pair(i, 1));
                }
            } else if (target_flow < 0) {
                if (variables[i].second < 0 && edge_diff[i / 2][i % 2] > -1) {
                    modified_variables.push_back(std::make_pair(i, -1));
                }
                if (variables[i].second > 0 && edge_diff[i / 2][i % 2] < 1) {
                    modified_variables.push_back(std::make_pair(i, 1));
                }
            }
        }
    }

    std::random_shuffle(modified_variables.begin(), modified_variables.end());

    for (int i = 0; i < abs(target_flow) / 2; ++i) {
        auto& info = modified_variables[i];
        edge_diff[info.first / 2][info.first % 2] += info.second;
    }

    for (int i = 0; i < face_edgeOrients.size(); ++i) {
        for (int j = 0; j < 3; ++j) {
            face_edgeOrients[i][j] =
                (face_edgeOrients[i][j] + disajoint_orient_tree.Orient(i)) % 4;
        }
    }

    //	WriteTestData();
}

void Parametrizer::ComputeMaxFlow() {
    //(std::vector<Vector3i>& FQ, std::vector<Vector3i>& F2E, std::vector<Vector2i>& E2F,
    // std::vector<Vector2i>& edge_diff, std::vector<int>& sing);
    std::vector<Vector2i> E2F(edge_diff.size(), Vector2i(-1, -1));
    for (int i = 0; i < face_edgeIds.size(); ++i) {
        for (int j = 0; j < 3; ++j) {
            int e = face_edgeIds[i][j];
            if (E2F[e][0] == -1)
                E2F[e][0] = i;
            else
                E2F[e][1] = i;
        }
    }
    hierarchy.DownsampleEdgeGraph(face_edgeOrients, face_edgeIds, E2F, edge_diff);
    Optimizer::optimize_integer_constraints(hierarchy, singularities);
    hierarchy.UpdateGraphValue(face_edgeOrients, face_edgeIds, E2F, edge_diff);
}

void Parametrizer::WriteTestData() {
    std::ofstream os("test.data");
    auto& F = hierarchy.mF;
    os << V.cols() << " " << edge_diff.size() << " " << F.cols() << "\n";
    for (int i = 0; i < edge_diff.size(); ++i) {
        os << edge_diff[i][0] << " " << edge_diff[i][1] << " ";
    }
    os << "\n";
    for (int i = 0; i < constraints_sign.size(); ++i) {
        os << constraints_sign[i][0] << " " << constraints_sign[i][1] << " "
           << constraints_sign[i][2] << "   ";
        os << constraints_index[i][0] << " " << constraints_index[i][1] << " "
           << constraints_index[i][2] << "   ";
        int diff = 0;
        for (int j = 0; j < 3; ++j) {
            diff += constraints_sign[i][j] *
                    edge_diff[constraints_index[i][j] / 2][constraints_index[i][j] % 2];
        }
        if (diff != 0) {
            //			printf("fail...\n");
        }
    }
    std::vector<std::set<int>> directions(edge_diff.size() * 2);
    for (int i = 0; i < constraints_sign.size() / 2; ++i) {
        os << -constraints_sign[i * 2][0] * constraints_sign[i * 2 + 1][2] << " "
           << constraints_index[i * 2][0] << " " << constraints_index[i * 2 + 1][2] << " "
           << constraints_sign[i * 2 + 1][0] * constraints_sign[i * 2][2] << " "
           << constraints_index[i * 2 + 1][0] << " " << constraints_index[i * 2][2] << "\n";
    }
    for (int i = 0; i < constraints_sign.size() / 2; ++i) {
        for (int j = 0; j < 3; ++j) {
            int v0 = edge_diff[constraints_index[i * 2][j] / 2][constraints_index[i * 2][j] % 2];
            int v1 = edge_diff[constraints_index[i * 2 + 1][(j + 2) % 3] / 2]
                              [constraints_index[i * 2 + 1][(j + 2) % 3] % 2];
            int v2 = edge_diff[constraints_index[i * 2 + 1][j] / 2]
                              [constraints_index[i * 2 + 1][j] % 2];
            int v3 = edge_diff[constraints_index[i * 2][(j + 2) % 3] / 2]
                              [constraints_index[i * 2][(j + 2) % 3] % 2];
            v0 = (v0 >= 0) ? 1 : -1;
            v1 = (v1 >= 0) ? 1 : -1;
            v2 = (v2 >= 0) ? 1 : -1;
            v3 = (v3 >= 0) ? 1 : -1;
            int sign1 = -constraints_sign[i * 2][j] * constraints_sign[i * 2 + 1][j];
            int sign2 =
                constraints_sign[i * 2 + 1][(j + 2) % 3] * constraints_sign[i * 2][(j + 2) % 3];
            if (v0 != 0 && v1 != 0) {
                directions[constraints_index[i * 2 + 1][(j + 2) % 3]].insert(v0 * sign1);
                directions[constraints_index[i * 2][j]].insert(v1 * sign1);
            }
            if (v2 != 0 && v3 != 0) {
                directions[constraints_index[i * 2 + 1][j]].insert(v3 * sign2);
                directions[constraints_index[i * 2][(j + 2) % 3]].insert(v2 * sign2);
            }
        }
    }
    int count[3] = {0};
    for (int i = 0; i < directions.size(); ++i) {
        count[directions[i].size()] += 1;
    }
    printf("count %d %d %d\n", count[0], count[1], count[2]);
    exit(0);
    for (int i = 0; i < edge_values.size(); ++i) {
        os << edge_values[i].x << " " << edge_values[i].y << " " << edge_diff[i][0] << " "
           << edge_diff[i][1] << "\n";
    }

    for (int i = 0; i < F.cols(); ++i) {
        os << F(0, i) << " " << F(1, i) << " " << F(2, i) << "\n";
    }
    os.close();
}

void Parametrizer::FixFlipAdvance() {
    auto& V = hierarchy.mV[0];
    auto& F = hierarchy.mF;
    std::vector<std::pair<int, int>> parent_edge(edge_values.size());
    std::vector<std::set<int>> edge_to_faces(edge_values.size());
    for (int i = 0; i < face_edgeIds.size(); ++i) {
        for (int j = 0; j < 3; ++j) {
            edge_to_faces[face_edgeIds[i][j]].insert(i);
        }
    }
    disajoint_tree = DisajointTree(V.cols());
    DisajointTree& tree = disajoint_tree;  //(V.cols());
    for (int i = 0; i < parent_edge.size(); ++i) {
        parent_edge[i] = std::make_pair(i, 0);
    }
    std::vector<std::unordered_map<int, std::list<int>>> vertices_to_edges(V.cols());
    for (int i = 0; i < F.cols(); ++i) {
        for (int j = 0; j < 3; ++j) {
            int v0 = F(j, i);
            int v1 = F((j + 1) % 3, i);
            int eid = face_edgeIds[i][j];  // edge_ids[DEdge(v0, v1)];
            std::list<int> l;
            l.push_back(eid);
            vertices_to_edges[v0].insert(std::make_pair(v1, l));
        }
    }
    int edge_len = 1;
    auto sanity = [&](int count) {
        printf("check sanity %d:\n", count);
        // parent_edge, tree, vertices_to_edge, edge_to_faces
        for (int i = 0; i < parent_edge.size(); ++i) {
            if (parent_edge[i].first == i) {
                int nx = tree.Parent(edge_values[i].x);
                int ny = tree.Parent(edge_values[i].y);
                if (nx == ny && Vector2i::Zero() == edge_diff[i]) continue;
                auto& l1 = vertices_to_edges[nx][ny];
                auto& l2 = vertices_to_edges[ny][nx];
                if (std::find(l1.begin(), l1.end(), i) == l1.end() ||
                    std::find(l2.begin(), l2.end(), i) == l2.end()) {
                    printf("edge %d not indexed in vertices (%d %d) %d %d\n", i, nx, ny,
                           edge_values[i].x, edge_values[i].y);
                    for (auto& m : l1) {
                        printf("<%d %d>  ", edge_values[m].x, edge_values[m].y);
                    }
                    printf("\n");
                    for (auto& m : l2) {
                        printf("<%d %d>  ", edge_values[m].x, edge_values[m].y);
                    }
                    printf("\n");
                    //                    exit(0);
                }
            }
        }
        for (int i = 0; i < V.cols(); ++i) {
            if (tree.Parent(i) != i && vertices_to_edges[i].size()) {
                printf("child edge list not empty!\n");
                //                exit(0);
            }
            for (auto& l : vertices_to_edges[i]) {
                if (tree.Parent(l.first) != l.first) {
                    printf("vertex index not root!\n");
                    //                    exit(0);
                }
                for (auto& li : l.second) {
                    /*					if (get_parents(parent_edge, li) != li) {
                                                                    printf("%d %d %d\n", i,
                    tree.Parent(i), li); printf("(%d %d): <%d %d> ==> <%d %d>\n", li,
                    get_parents(parent_edge, li), edge_values[li].x, edge_values[li].y,
                                                                            edge_values[get_parents(parent_edge,
                    li)].x, edge_values[get_parents(parent_edge, li)].y); printf("edge index not
                    root!\n");
                    //                        exit(0);
                                                            }*/
                    if (tree.Parent(edge_values[li].x) == tree.Parent(edge_values[li].y) &&
                        edge_diff[li] == Vector2i::Zero()) {
                        printf("%d %d %d %d\n", i, l.first, edge_values[li].x, edge_values[li].y);
                        printf("zero edge length!\n");
                        //                        exit(0);
                    }
                }
            }
        }
        std::vector<std::vector<int>> faces_from_edge(F.cols());
        for (int i = 0; i < edge_to_faces.size(); ++i) {
            for (auto& f : edge_to_faces[i]) {
                faces_from_edge[f].push_back(i);
            }
        }
        for (int f = 0; f < F.cols(); ++f) {
            std::set<int> l;
            for (int j = 0; j < 3; ++j) {
                int v1 = tree.Parent(F(j, f));
                int v2 = tree.Parent(F((j + 1) % 3, f));
                if (v1 == v2 &&
                    edge_diff[get_parents(parent_edge, face_edgeIds[f][j])] == Vector2i::Zero()) {
                    l.clear();
                    break;
                }
                int pid = get_parents(parent_edge, face_edgeIds[f][j]);
                l.insert(pid);
            }

            if (l.size() != faces_from_edge[f].size()) {
                printf("inconsistent edge-face connection! -1 %d %d\n", (int)l.size(),
                       (int)faces_from_edge[f].size());
                for (auto& p : l) {
                    printf("%d ", p);
                }
                printf("\n");
                for (int i = 0; i < faces_from_edge[f].size(); ++i) {
                    printf("%d ", faces_from_edge[f][i]);
                }
                printf("\n");
                printf("face %d %d %d %d\n", f, tree.Parent(F(0, f)), tree.Parent(F(1, f)),
                       tree.Parent(F(2, f)));
                printf("face origin %d %d %d\n", F(0, f), F(1, f), F(2, f));
                //                exit(0);
            }
            int i = 0;
            for (auto& p : l) {
                if (p != faces_from_edge[f][i++]) {
                    printf("inconsistent edge-face connection! %d\n", f);
                    //                    exit(0);
                }
            }
        }
        // check diff
        int total_area = 0;
        for (int i = 0; i < F.cols(); ++i) {
            Vector2i diff[3];
            int orients[3];
            int pids[3];
            for (int j = 0; j < 3; ++j) {
                int eid = face_edgeIds[i][j];
                int orient = face_edgeOrients[i][j];
                int pid = get_parents(parent_edge, eid);
                pids[j] = pid;
                int parent_orient = get_parents_orient(parent_edge, eid);
                diff[j] = rshift90(edge_diff[pid], (orient + parent_orient) % 4);
                orients[j] = (orient + parent_orient) % 4;
            }
            Vector2i total = diff[0] + diff[1] + diff[2];
            if (total != Vector2i::Zero()) {
                printf("zero face constraint violated %d\n", i);
                printf("<%d %d> (%d eid %d) <%d %d> (%d eid %d) <%d %d> (%d eid %d)\n", diff[0][0],
                       diff[0][1], orients[0], pids[0], diff[1][0], diff[1][1], orients[1],
                       pids[1], diff[2][0], diff[2][1], orients[2], pids[2]);
                printf("f %d (%d %d %d):  %d %d %d", i, tree.Parent(F(0, i)), tree.Parent(F(1, i)),
                       tree.Parent(F(2, i)), get_parents(parent_edge, face_edgeIds[i][0]),
                       get_parents(parent_edge, face_edgeIds[i][1]),
                       get_parents(parent_edge, face_edgeIds[i][2]));
                //                exit(0);
            }
            int area = -diff[0][0] * diff[2][1] + diff[0][1] * diff[2][0];
            if (area < 0) total_area -= area;
        }
        printf("total minus area: %d\n", total_area);
        printf("finish...\n");
    };

    auto ExtractEdgeSet = [&](int v1, int v2, int pid,
                              std::vector<std::pair<int, Vector2i>>& edge_change) {
        std::unordered_map<int, Vector2i> edge_set;
        edge_change.push_back(std::make_pair(pid, edge_diff[pid]));
        edge_set.insert(edge_change.back());
        std::queue<int> faces;
        for (auto& f : edge_to_faces[pid]) {
            faces.push(f);
        }
        std::set<int> modified;
        while (!faces.empty()) {
            int f = faces.front();
            modified.insert(f);
            faces.pop();
            int eids[3];
            int orient[3];
            Vector2i total_diff(0, 0);
            for (int i = 0; i < 3; ++i) {
                int eid = face_edgeIds[f][i];
                int pid = get_parents(parent_edge, eid);
                orient[i] = (get_parents_orient(parent_edge, eid) + face_edgeOrients[f][i]) % 4;
                eids[i] = pid;
                Vector2i diff = edge_diff[pid];
                if (edge_set.count(pid)) diff -= edge_set[pid];
                total_diff += rshift90(diff, orient[i]);
            }
            int count = 0;
            for (int i = 0; i < 3; ++i) {
                if (!((tree.Parent(edge_values[eids[i]].x) != v1 &&
                       tree.Parent(edge_values[eids[i]].y) != v1) ||
                      edge_set.count(eids[i]))) {
                    count += 1;
                }
            }
            int next_e = 0;
            while ((tree.Parent(edge_values[eids[next_e]].x) != v1 &&
                    tree.Parent(edge_values[eids[next_e]].y) != v1) ||
                   edge_set.count(eids[next_e])) {
                next_e += 1;
                if (next_e == 3) break;
            }
            if (total_diff == Vector2i::Zero()) {
                continue;
            }
            if (next_e == 3) {
                edge_change.clear();
                return;
            }
            int e = next_e + 1;
            while (e < 3 && eids[next_e] != eids[e]) e += 1;
            if (e != 3) {
                edge_change.clear();
                return;
            }
            int change_pid = eids[next_e];
            auto new_diff = rshift90(total_diff, (4 - orient[next_e]) % 4);
            if (abs(edge_diff[change_pid][0] - new_diff[0]) > edge_len ||
                abs(edge_diff[change_pid][1] - new_diff[1]) > edge_len) {
                edge_change.clear();
                return;
            }
            edge_change.push_back(std::make_pair(change_pid, new_diff));
            edge_set.insert(edge_change.back());
            for (auto& nf : edge_to_faces[change_pid]) {
                if (nf != f) {
                    faces.push(nf);
                }
            }
        }
    };
    float sum_t1 = 0, sum_t2 = 0, sum_t3 = 0, sum_t4 = 0;
    auto collapse = [&](int v1, int v2) {
        if (v1 == v2) return;
        int t1, t2;
        t1 = GetCurrentTime64();
        std::set<int> collapsed_faces;
        for (auto& collapsed_edge : vertices_to_edges[v1][v2]) {
            if (edge_diff[collapsed_edge] == Vector2i::Zero()) {
                collapsed_faces.insert(edge_to_faces[collapsed_edge].begin(),
                                       edge_to_faces[collapsed_edge].end());
                edge_to_faces[collapsed_edge].clear();
            }
        }
        t2 = GetCurrentTime64();
        sum_t1 += (t2 - t1) * 1e-3;
        t1 = GetCurrentTime64();
        for (auto& l : vertices_to_edges[v1]) {
            auto it0 = vertices_to_edges[l.first].find(v1);
            std::pair<int, std::list<int>> rec = *it0;
            rec.first = v2;
            int next_m = l.first;
            if (next_m != v1) {
                vertices_to_edges[next_m].erase(it0);
            } else {
                next_m = v2;
            }
            std::list<int> neighbor_edges;
            for (auto& li : l.second) {
                if (edge_diff[li] != Vector2i::Zero() || l.first != v2) {
                    neighbor_edges.push_back(li);
                } else {
                }
            }
            auto it = vertices_to_edges[v2].find(next_m);
            if (it != vertices_to_edges[v2].end()) {
                for (auto& li : neighbor_edges) {
                    it->second.push_back(li);
                    vertices_to_edges[next_m][v2].push_back(li);
                }
            } else {
                if (neighbor_edges.size()) vertices_to_edges[v2][next_m] = neighbor_edges;
                if (next_m != v2) vertices_to_edges[next_m].insert(rec);
            }
        }
        tree.MergeFromTo(v1, v2);
        t2 = GetCurrentTime64();
        sum_t2 += (t2 - t1) * 1e-3;
        t1 = GetCurrentTime64();


        std::set<int> modified_faces;
        for (auto& f : collapsed_faces) {
            for (int j = 0; j < 3; ++j) {
                int vv0 = tree.Parent(F(j, f));
                int vv1 = tree.Parent(F((j + 1) % 3, f));
                if (vv0 != vv1 ||
                    edge_diff[get_parents(parent_edge, face_edgeIds[f][j])] != Vector2i::Zero()) {
                    int eid = face_edgeIds[f][j];
                    int peid = get_parents(parent_edge, eid);
                    while (true) {
                        bool update = false;
                        for (auto& nf : edge_to_faces[peid]) {
                            if (nf != f) continue;
                            int non_collapse = 0;
                            for (int nj = 0; nj < 3; ++nj) {
                                if (edge_diff[get_parents(parent_edge, face_edgeIds[nf][nj])] !=
                                    Vector2i::Zero()) {
                                    non_collapse += 1;
                                }
                            }
                            if (non_collapse == 3) continue;
                            for (int nj = 0; nj < 3; ++nj) {
                                int nv0 = tree.Parent(F(nj, nf));
                                int nv1 = tree.Parent(F((nj + 1) % 3, nf));
                                if (nv0 != nv1 ||
                                    edge_diff[get_parents(parent_edge, face_edgeIds[nf][nj])] !=
                                        Vector2i::Zero()) {
                                    int neid = face_edgeIds[nf][nj];
                                    int npeid = get_parents(parent_edge, neid);
                                    if (npeid != peid && DEdge(nv0, nv1) == DEdge(vv0, vv1)) {
                                        modified_faces.insert(nf);
                                        update = true;
                                        int orient = 0;
                                        auto diff1 = edge_diff[peid];
                                        auto diff2 = edge_diff[npeid];
                                        while (orient < 4 && rshift90(diff1, orient) != diff2)
                                            orient += 1;
                                        if (orient == 4) {
                                            printf("v %d %d %d %d\n", F(j, f), F((j + 1) % 3, f),
                                                   F(nj, nf), F((nj + 1) % 3, nf));
                                            printf("edge fail to collapse %d %d %d %d\n",
                                                   edge_values[peid].x, edge_values[peid].y,
                                                   edge_values[npeid].x, edge_values[npeid].y);
                                            printf("%d %d %d %d\n", diff1[0], diff1[1], diff2[0],
                                                   diff2[1]);
                                            printf("no orient solution!\n");
                                            exit(0);
                                        } else {
                                            parent_edge[npeid] = std::make_pair(peid, orient);
                                        }
                                        for (auto& p : edge_to_faces[npeid]) {
                                            edge_to_faces[peid].insert(p);
                                        }
                                        edge_to_faces[peid].erase(nf);
                                        edge_to_faces[npeid].clear();
                                        auto& l1 = vertices_to_edges[nv0][nv1];
                                        auto it = std::find(l1.begin(), l1.end(), npeid);
                                        if (it != l1.end()) l1.erase(it);
                                        auto& l2 = vertices_to_edges[nv1][nv0];
                                        it = std::find(l2.begin(), l2.end(), npeid);
                                        if (it != l2.end()) l2.erase(it);
                                        break;
                                    }
                                }
                            }
                            if (update) {
                                break;
                            }
                        }
                        if (!update) break;
                    }
                }
            }
        }
        t2 = GetCurrentTime64();
        sum_t3 += (t2 - t1) * 1e-3;
        t1 = GetCurrentTime64();
        for (auto& f : collapsed_faces) {
            for (int i = 0; i < 3; ++i) {
                int peid = get_parents(parent_edge, face_edgeIds[f][i]);
                edge_to_faces[peid].erase(f);
            }
        }
        vertices_to_edges[v1].clear();
        t2 = GetCurrentTime64();
        sum_t4 += (t2 - t1) * 1e-3;
    };

    auto CheckMove = [&](int v1, int v2, int pid, int check_face) {
        std::vector<std::pair<int, Vector2i>> edge_change;
        ExtractEdgeSet(v1, v2, pid, edge_change);
        if (edge_change.size() == 0) {
            return false;
        }
        // check face area
        std::set<int> modified_faces;
        for (auto& e : edge_change) {
            for (auto& f : edge_to_faces[e.first]) modified_faces.insert(f);
        }
        int original_face_area = 0, current_face_area = 0;
        for (auto& f : modified_faces) {
            int eid0 = face_edgeIds[f][0];
            int pid0 = get_parents(parent_edge, eid0);
            int eid1 = face_edgeIds[f][2];
            int pid1 = get_parents(parent_edge, eid1);
            int orient0 = (get_parents_orient(parent_edge, eid0) + face_edgeOrients[f][0]) % 4;
            int orient1 = (get_parents_orient(parent_edge, eid1) + face_edgeOrients[f][2]) % 4;
            Vector2i diff1 = rshift90(edge_diff[pid0], orient0);
            Vector2i diff2 = rshift90(edge_diff[pid1], orient1);
            int area = -diff1[0] * diff2[1] + diff1[1] * diff2[0];
            if (area < 0) original_face_area -= area;
        }

        // apply modify
        for (auto& p : edge_change) {
            edge_diff[p.first] -= p.second;
        }

        for (auto& f : modified_faces) {
            int eid0 = face_edgeIds[f][0];
            int pid0 = get_parents(parent_edge, eid0);
            int eid1 = face_edgeIds[f][2];
            int pid1 = get_parents(parent_edge, eid1);
            int orient0 = (get_parents_orient(parent_edge, eid0) + face_edgeOrients[f][0]) % 4;
            int orient1 = (get_parents_orient(parent_edge, eid1) + face_edgeOrients[f][2]) % 4;
            Vector2i diff1 = rshift90(edge_diff[pid0], orient0);
            Vector2i diff2 = rshift90(edge_diff[pid1], orient1);
            int area = -diff1[0] * diff2[1] + diff1[1] * diff2[0];
            if (area < 0) current_face_area -= area;
        }
        // reverse modify
        if (current_face_area < original_face_area || !check_face) {
            for (auto& p : edge_change) {
                if (edge_diff[p.first] == Vector2i::Zero()) {
                    collapse(tree.Parent(edge_values[p.first].x),
                             tree.Parent(edge_values[p.first].y));
                }
            }
            return true;
        } else {
            for (auto& p : edge_change) {
                edge_diff[p.first] += p.second;
            }
            return false;
        }
    };

    int t1 = GetCurrentTime64();
    for (int i = 0; i < edge_diff.size(); ++i) {
        if (edge_diff[i] == Vector2i::Zero()) {
            collapse(tree.Parent(edge_values[i].x), tree.Parent(edge_values[i].y));
        }
    }
    int t2 = GetCurrentTime64();
    printf("Collapse Use time %lf <%lf %lf %lf %lf>\n", (t2 - t1) * 1e-3, sum_t1, sum_t2, sum_t3, sum_t4);

    for (; edge_len < 2; edge_len += 1) {
        int counter = 0;
        while (true) {
            counter++;
            bool update = false;
            for (int i = 0; i < parent_edge.size(); ++i) {
                if (i == parent_edge[i].first) {
                    if (edge_len > 1 && edge_around_singularities.count(i)) continue;
                    int p1 = tree.Parent(edge_values[i].x);
                    int p2 = tree.Parent(edge_values[i].y);
                    if (p1 == p2) continue;
                    if (CheckMove(p1, p2, i, 1)) {
                        update = true;
                    } else {
                        if (CheckMove(p2, p1, i, 1)) {
                            update = true;
                        }
                    }
                }
            }
            if (!update) break;
        }
        if (edge_len == 1) {
            std::set<int> edge_parent;
            for (auto& e : edge_around_singularities) {
                edge_parent.insert(get_parents(parent_edge, e));
            }
            std::swap(edge_parent, edge_around_singularities);
            break;
        }
    }

    int total_area = 0;
    for (int i = 0; i < F.cols(); ++i) {
        Vector2i diff[3];
        int eid[3];
        int orient[3];
        for (int j = 0; j < 3; ++j) {
            int e = face_edgeIds[i][j];
            int p = get_parents(parent_edge, e);
            eid[j] = p;
            orient[j] = (get_parents_orient(parent_edge, e) + face_edgeOrients[i][j]) % 4;
            diff[j] = edge_diff[p];
        }
        Vector2i d1 = rshift90(diff[0], orient[0]);
        Vector2i d2 = rshift90(-diff[2], orient[2]);
        int area = d1[0] * d2[1] - d1[1] * d2[0];
        if (area < 0) {
            for (int j = 0; j < 3; ++j) {
                CheckMove(tree.Parent(F(j, i)), tree.Parent(F((j + 1) % 3, i)), eid[j], 1);
                CheckMove(tree.Parent(F((j + 1) % 3, i)), tree.Parent(F(j, i)), eid[j], 1);
            }
            total_area -= area;
        }
    }

    std::vector<int> bad_vertices(vertices_to_edges.size(), 0);
    for (int i = 0; i < vertices_to_edges.size(); ++i) {
        if (i != tree.Parent(i)) continue;
        int counters = 0;
        for (auto& p : vertices_to_edges[i]) {
            if (p.first == i) continue;
            int q = 0;
            for (auto& l : p.second) {
                if (edge_diff[l][0] == 0 || edge_diff[l][1] == 0) q = 1;
            }
            if (q == 1) {
                counters += 1;
            }
        }
        if (counters < 3) {
            bad_vertices[i] = 1;
        }
    }
    while (true) {
        bool update = false;
        for (int i = 0; i < vertices_to_edges.size(); ++i) {
            if (bad_vertices[i] == 0) continue;
            std::unordered_map<int, std::list<int>> collapse_set;
            for (auto& p : vertices_to_edges[i]) {
                if (bad_vertices[p.first]) {
                    continue;
                }
                collapse_set.insert(p);
            }
            for (auto& p : collapse_set) {
                for (auto& q : p.second) {
                    if (CheckMove(i, p.first, q, 0)) {
                        bad_vertices[i] = 0;
                        update = true;
                        break;
                    }
                }
            }
        }
        if (!update) break;
    }

    for (int i = 0; i < parent_edge.size(); ++i) {
        int orient = get_parents_orient(parent_edge, i);
        int p = get_parents(parent_edge, i);
        edge_diff[i] = rshift90(edge_diff[p], orient);
    }
}

void Parametrizer::ExtractMesh(const char* obj_name) {
    std::vector<int> compact_answer(bad_vertices.size());
    compact_answer[0] = 1 - bad_vertices[0];
    for (int i = 1; i < bad_vertices.size(); ++i) {
        compact_answer[i] = compact_answer[i - 1] + (1 - bad_vertices[i]);
    }
    std::ofstream os(obj_name);
    for (int i = 0; i < bad_vertices.size(); ++i) {
        if (bad_vertices[i]) continue;
        os << "v " << O_compact[i][0] << " " << O_compact[i][1] << " " << O_compact[i][2] << "\n";
    }
    for (int i = 0; i < F_compact.size(); ++i) {
        os << "f " << compact_answer[F_compact[i][0]] << " " << compact_answer[F_compact[i][1]]
           << " " << compact_answer[F_compact[i][2]] << " " << compact_answer[F_compact[i][3]]
           << "\n";
    }
    os.close();
}
