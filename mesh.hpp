#ifndef DYNEARTHSOL3D_MESH_HPP
#define DYNEARTHSOL3D_MESH_HPP

void points_to_new_mesh(const Param &param, int npoints, const double *points,
                        int n_init_segments, const int *init_segments, const int *init_segflags,
                        double max_elem_size, int vertex_per_polygon,
                        int &nnode, int &nelem, int &nseg,
                        double *&pcoord, int *&pconnectivity,
                        int *&psegment, int *&psegflag);
void points_to_mesh(const Param &param, Variables &var,
                    int npoints, const double *points,
                    int n_init_segments, const int *init_segments, const int *init_segflags,
                    double max_elem_size, int vertex_per_polygon);
void create_boundary_flags(Variables& var);
void create_boundary_facets(Variables& var);
void create_support(Variables& var);
void create_elem_groups(Variables& var);
void create_new_mesh(const Param&, Variables&);

#endif
