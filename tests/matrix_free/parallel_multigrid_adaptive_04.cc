// ---------------------------------------------------------------------
//
// Copyright (C) 2016 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE at
// the top level of the deal.II distribution.
//
// ---------------------------------------------------------------------



// same as parallel_multigird_adaptive_02, but using mg::SmootherRelaxation
// rather than MGSmootherPrecondition

#include "../tests.h"

#include <deal.II/base/logstream.h>
#include <deal.II/base/utilities.h>
#include <deal.II/lac/la_parallel_vector.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/mapping_q.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/distributed/tria.h>

#include <deal.II/multigrid/multigrid.h>
#include <deal.II/multigrid/mg_transfer_matrix_free.h>
#include <deal.II/multigrid/mg_tools.h>
#include <deal.II/multigrid/mg_coarse.h>
#include <deal.II/multigrid/mg_smoother.h>
#include <deal.II/multigrid/mg_matrix.h>

#include <deal.II/matrix_free/operators.h>
#include <deal.II/matrix_free/matrix_free.h>
#include <deal.II/matrix_free/fe_evaluation.h>

std::ofstream logfile("output");

using namespace dealii::MatrixFreeOperators;


template <int dim, int fe_degree, int n_q_points_1d = fe_degree+1, typename number=double>
class LaplaceOperator : public MatrixFreeOperators::Base<dim, number>
{
public:
  typedef number value_type;

  LaplaceOperator()
    :
    MatrixFreeOperators::Base<dim, number>()
  {};

  void compute_diagonal ()
  {
    unsigned int dummy = 0;
    LinearAlgebra::distributed::Vector<number> &inverse_diagonal_entries = Base<dim,number>::inverse_diagonal_entries;
    this->initialize_dof_vector(inverse_diagonal_entries);
    Base<dim,number>::
    data->cell_loop (&LaplaceOperator::local_diagonal_cell,
                     this, inverse_diagonal_entries, dummy);

    const std::vector<unsigned int> &
    constrained_dofs = Base<dim,number>::data->get_constrained_dofs();
    for (unsigned int i=0; i<constrained_dofs.size(); ++i)
      inverse_diagonal_entries.local_element(constrained_dofs[i]) = 0.;
    for (unsigned int i=0; i<Base<dim,number>::edge_constrained_indices.size(); ++i)
      inverse_diagonal_entries.local_element(Base<dim,number>::edge_constrained_indices[i]) = 0.;

    for (unsigned int i=0; i<inverse_diagonal_entries.local_size(); ++i)
      if (std::abs(inverse_diagonal_entries.local_element(i)) > 1e-10)
        inverse_diagonal_entries.local_element(i) = 1./inverse_diagonal_entries.local_element(i);
      else
        inverse_diagonal_entries.local_element(i) = 1.;
  }

protected:


  void apply_add (LinearAlgebra::distributed::Vector<number>       &dst,
                  const LinearAlgebra::distributed::Vector<number> &src) const
  {
    Base<dim,number>::
    data->cell_loop (&LaplaceOperator::local_apply, this, dst, src);
  }


private:

  void
  local_apply (const MatrixFree<dim,number>                &data,
               LinearAlgebra::distributed::Vector<number>       &dst,
               const LinearAlgebra::distributed::Vector<number> &src,
               const std::pair<unsigned int,unsigned int>  &cell_range) const
  {
    FEEvaluation<dim,fe_degree,n_q_points_1d,1,number> phi (data);

    for (unsigned int cell=cell_range.first; cell<cell_range.second; ++cell)
      {
        phi.reinit (cell);
        phi.read_dof_values(src);
        phi.evaluate (false,true,false);
        for (unsigned int q=0; q<phi.n_q_points; ++q)
          phi.submit_gradient (phi.get_gradient(q), q);
        phi.integrate (false,true);
        phi.distribute_local_to_global (dst);
      }
  }

  void
  local_diagonal_cell (const MatrixFree<dim,number>                &data,
                       LinearAlgebra::distributed::Vector<number>       &dst,
                       const unsigned int &,
                       const std::pair<unsigned int,unsigned int>  &cell_range) const
  {
    FEEvaluation<dim,fe_degree,n_q_points_1d,1,number> phi (data);

    for (unsigned int cell=cell_range.first; cell<cell_range.second; ++cell)
      {
        phi.reinit (cell);

        VectorizedArray<number> local_diagonal_vector[phi.tensor_dofs_per_cell];
        for (unsigned int i=0; i<phi.dofs_per_cell; ++i)
          {
            for (unsigned int j=0; j<phi.dofs_per_cell; ++j)
              phi.begin_dof_values()[j] = VectorizedArray<number>();
            phi.begin_dof_values()[i] = 1.;
            phi.evaluate (false,true,false);
            for (unsigned int q=0; q<phi.n_q_points; ++q)
              phi.submit_gradient (phi.get_gradient(q), q);
            phi.integrate (false,true);
            local_diagonal_vector[i] = phi.begin_dof_values()[i];
          }
        for (unsigned int i=0; i<phi.tensor_dofs_per_cell; ++i)
          phi.begin_dof_values()[i] = local_diagonal_vector[i];
        phi.distribute_local_to_global (dst);
      }
  }

};



template <typename LAPLACEOPERATOR>
class MGInterfaceMatrix : public Subscriptor
{
public:
  void initialize (const LAPLACEOPERATOR &laplace)
  {
    this->laplace = &laplace;
  }

  void vmult (LinearAlgebra::distributed::Vector<double> &dst,
              const LinearAlgebra::distributed::Vector<double> &src) const
  {
    laplace->vmult_interface_down(dst, src);
  }

  void Tvmult (LinearAlgebra::distributed::Vector<double> &dst,
               const LinearAlgebra::distributed::Vector<double> &src) const
  {
    laplace->vmult_interface_up(dst, src);
  }

private:
  SmartPointer<const LAPLACEOPERATOR> laplace;
};



template <int dim, typename LAPLACEOPERATOR>
class MGTransferMF : public MGTransferMatrixFree<dim, typename LAPLACEOPERATOR::value_type>
{
public:
  MGTransferMF(const MGLevelObject<LAPLACEOPERATOR> &laplace,
               const MGConstrainedDoFs &mg_constrained_dofs)
    :
    MGTransferMatrixFree<dim, typename LAPLACEOPERATOR::value_type>(mg_constrained_dofs),
    laplace_operator (laplace)
  {
  }

  /**
   * Overload copy_to_mg from MGTransferPrebuilt to get the vectors compatible
   * with MatrixFree and bypass the crude vector initialization in
   * MGTransferPrebuilt
   */
  template <class InVector, int spacedim>
  void
  copy_to_mg (const DoFHandler<dim,spacedim> &mg_dof_handler,
              MGLevelObject<LinearAlgebra::distributed::Vector<typename LAPLACEOPERATOR::value_type> > &dst,
              const InVector &src) const
  {
    for (unsigned int level=dst.min_level();
         level<=dst.max_level(); ++level)
      laplace_operator[level].initialize_dof_vector(dst[level]);
    MGTransferMatrixFree<dim, typename LAPLACEOPERATOR::value_type>::
    copy_to_mg(mg_dof_handler, dst, src);
  }

private:
  const MGLevelObject<LAPLACEOPERATOR> &laplace_operator;
};



template<typename MatrixType, typename Number>
class MGCoarseIterative : public MGCoarseGridBase<LinearAlgebra::distributed::Vector<Number> >
{
public:
  MGCoarseIterative() {}

  void initialize(const MatrixType &matrix)
  {
    coarse_matrix = &matrix;
  }

  virtual void operator() (const unsigned int   level,
                           LinearAlgebra::distributed::Vector<double> &dst,
                           const LinearAlgebra::distributed::Vector<double> &src) const
  {
    ReductionControl solver_control (1e4, 1e-50, 1e-10);
    SolverCG<LinearAlgebra::distributed::Vector<double> > solver_coarse (solver_control);
    solver_coarse.solve (*coarse_matrix, dst, src, PreconditionIdentity());
  }

  const MatrixType *coarse_matrix;
};




template <int dim, int fe_degree, int n_q_points_1d, typename number>
void do_test (const DoFHandler<dim>  &dof)
{
  if (types_are_equal<number,float>::value == true)
    {
      deallog.push("float");
      deallog.threshold_double(1e-6);
    }
  else
    {
      deallog.threshold_double(5.e-11);
    }

  deallog << "Testing " << dof.get_fe().get_name();
  deallog << std::endl;
  deallog << "Number of degrees of freedom: " << dof.n_dofs() << std::endl;

  IndexSet locally_relevant_dofs;
  DoFTools::extract_locally_relevant_dofs(dof, locally_relevant_dofs);

  // Dirichlet BC
  ZeroFunction<dim> zero_function;
  typename FunctionMap<dim>::type dirichlet_boundary;
  dirichlet_boundary[0] = &zero_function;

  // fine-level constraints
  ConstraintMatrix constraints;
  constraints.reinit(locally_relevant_dofs);
  DoFTools::make_hanging_node_constraints(dof, constraints);
  VectorTools::interpolate_boundary_values(dof, dirichlet_boundary,
                                           constraints);
  constraints.close();

  // level constraints:
  MGConstrainedDoFs mg_constrained_dofs;
  mg_constrained_dofs.initialize(dof, dirichlet_boundary);

  MappingQ<dim> mapping(fe_degree+1);

  LaplaceOperator<dim,fe_degree,n_q_points_1d,number> fine_matrix;
  MatrixFree<dim,number> fine_level_data;

  typename MatrixFree<dim,number>::AdditionalData fine_level_additional_data;
  fine_level_additional_data.tasks_parallel_scheme = MatrixFree<dim,number>::AdditionalData::none;
  fine_level_additional_data.tasks_block_size = 3;
  fine_level_additional_data.mpi_communicator = MPI_COMM_WORLD;
  fine_level_data.reinit (mapping, dof, constraints, QGauss<1>(n_q_points_1d),
                          fine_level_additional_data);

  fine_matrix.initialize(fine_level_data);
  fine_matrix.compute_diagonal();


  LinearAlgebra::distributed::Vector<number> in, sol;
  fine_matrix.initialize_dof_vector(in);
  fine_matrix.initialize_dof_vector(sol);

  // set constant rhs vector
  {
    // this is to make it consistent with parallel_multigrid_adaptive.cc
    ConstraintMatrix hanging_node_constraints;
    hanging_node_constraints.reinit(locally_relevant_dofs);
    DoFTools::make_hanging_node_constraints(dof, hanging_node_constraints);
    hanging_node_constraints.close();

    for (unsigned int i=0; i<in.local_size(); ++i)
      if (!hanging_node_constraints.is_constrained(in.get_partitioner()->local_to_global(i)))
        in.local_element(i) = 1.;
  }

  // set up multigrid in analogy to step-37
  typedef LaplaceOperator<dim,fe_degree,n_q_points_1d,number> LevelMatrixType;

  MGLevelObject<LevelMatrixType> mg_matrices;
  MGLevelObject<MatrixFree<dim,number> > mg_level_data;
  mg_matrices.resize(0, dof.get_triangulation().n_global_levels()-1);
  mg_level_data.resize(0, dof.get_triangulation().n_global_levels()-1);
  for (unsigned int level = 0; level<dof.get_triangulation().n_global_levels(); ++level)
    {
      typename MatrixFree<dim,number>::AdditionalData mg_additional_data;
      mg_additional_data.tasks_parallel_scheme = MatrixFree<dim,number>::AdditionalData::none;
      mg_additional_data.tasks_block_size = 3;
      mg_additional_data.mpi_communicator = MPI_COMM_WORLD;
      mg_additional_data.level_mg_handler = level;

      ConstraintMatrix level_constraints;
      IndexSet relevant_dofs;
      DoFTools::extract_locally_relevant_level_dofs(dof, level,
                                                    relevant_dofs);
      level_constraints.reinit(relevant_dofs);
      level_constraints.add_lines(mg_constrained_dofs.get_boundary_indices(level));
      level_constraints.close();

      mg_level_data[level].reinit (mapping, dof, level_constraints, QGauss<1>(n_q_points_1d),
                                   mg_additional_data);
      mg_matrices[level].initialize(mg_level_data[level],
                                    mg_constrained_dofs,
                                    level);
      mg_matrices[level].compute_diagonal();
    }
  MGLevelObject<MGInterfaceMatrix<LevelMatrixType> > mg_interface_matrices;
  mg_interface_matrices.resize(0, dof.get_triangulation().n_global_levels()-1);
  for (unsigned int level=0; level<dof.get_triangulation().n_global_levels(); ++level)
    mg_interface_matrices[level].initialize(mg_matrices[level]);

  MGTransferMF<dim,LevelMatrixType> mg_transfer(mg_matrices,
                                                mg_constrained_dofs);
  mg_transfer.build(dof);

  MGCoarseIterative<LevelMatrixType,number> mg_coarse;
  mg_coarse.initialize(mg_matrices[0]);

  typedef PreconditionChebyshev<LevelMatrixType,LinearAlgebra::distributed::Vector<number> > SMOOTHER;
  mg::SmootherRelaxation<SMOOTHER, LinearAlgebra::distributed::Vector<number> >
  mg_smoother;

  MGLevelObject<typename SMOOTHER::AdditionalData> smoother_data;
  smoother_data.resize(0, dof.get_triangulation().n_global_levels()-1);
  for (unsigned int level = 0; level<dof.get_triangulation().n_global_levels(); ++level)
    {
      smoother_data[level].smoothing_range = 15.;
      smoother_data[level].degree = 5;
      smoother_data[level].eig_cg_n_iterations = 15;
      smoother_data[level].matrix_diagonal_inverse =
        mg_matrices[level].get_matrix_diagonal_inverse();
    }
  mg_smoother.initialize(mg_matrices, smoother_data);

  mg::Matrix<LinearAlgebra::distributed::Vector<double> >
  mg_matrix(mg_matrices);
  mg::Matrix<LinearAlgebra::distributed::Vector<double> >
  mg_interface(mg_interface_matrices);

  Multigrid<LinearAlgebra::distributed::Vector<double> > mg(dof,
                                                            mg_matrix,
                                                            mg_coarse,
                                                            mg_transfer,
                                                            mg_smoother,
                                                            mg_smoother);
  mg.set_edge_matrices(mg_interface, mg_interface);
  PreconditionMG<dim, LinearAlgebra::distributed::Vector<double>,
                 MGTransferMF<dim,LevelMatrixType> >
                 preconditioner(dof, mg, mg_transfer);

  {
    ReductionControl control(30, 1e-20, 1e-7);
    SolverCG<LinearAlgebra::distributed::Vector<double> > solver(control);
    solver.solve(fine_matrix, sol, in, preconditioner);
  }

  if (types_are_equal<number,float>::value == true)
    deallog.pop();

  fine_matrix.clear();
  for (unsigned int level = 0; level<dof.get_triangulation().n_global_levels(); ++level)
    mg_matrices[level].clear();
}



template <int dim, int fe_degree>
void test ()
{
  parallel::distributed::Triangulation<dim> tria(MPI_COMM_WORLD,
                                                 Triangulation<dim>::limit_level_difference_at_vertices,
                                                 parallel::distributed::Triangulation<dim>::construct_multigrid_hierarchy);
  GridGenerator::hyper_cube (tria);
  tria.refine_global(6-dim);
  const unsigned int n_runs = fe_degree == 1 ? 6-dim : 5-dim;
  for (unsigned int i=0; i<n_runs; ++i)
    {
      for (typename Triangulation<dim>::active_cell_iterator cell=tria.begin_active();
           cell != tria.end(); ++cell)
        if (cell->is_locally_owned() &&
            (cell->center().norm() < 0.5 && (cell->level() < 5 ||
                                             cell->center().norm() > 0.45)
             ||
             (dim == 2 && cell->center().norm() > 1.2)))
          cell->set_refine_flag();
      tria.execute_coarsening_and_refinement();
      FE_Q<dim> fe (fe_degree);
      DoFHandler<dim> dof (tria);
      dof.distribute_dofs(fe);
      dof.distribute_mg_dofs(fe);

      do_test<dim, fe_degree, fe_degree+1, double> (dof);
    }
}



int main (int argc, char **argv)
{
  Utilities::MPI::MPI_InitFinalize mpi_init(argc, argv);

  if (Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) == 0)
    {
      deallog.attach(logfile);
      deallog << std::setprecision (4);
    }

  {
    deallog.threshold_double(1.e-10);
    deallog.push("2d");
    test<2,1>();
    test<2,3>();
    deallog.pop();
    deallog.push("3d");
    test<3,1>();
    test<3,2>();
    deallog.pop();
  }
}