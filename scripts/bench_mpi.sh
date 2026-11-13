#!/bin/sh
# Open MPI comparison: only runs where mpirun exists (Linux cluster / CI).
if ! command -v mpirun >/dev/null 2>&1; then echo '{"mpi":"skipped: mpirun not found on this machine"}'; exit 0; fi
cat > /tmp/mpi_allreduce.c <<'EOF'
#include <mpi.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){MPI_Init(&argc,&argv);int r;MPI_Comm_rank(MPI_COMM_WORLD,&r);
size_t n=16777216/4;float*b=malloc(n*4);for(size_t i=0;i<n;i++)b[i]=r+1;
double t=MPI_Wtime();for(int i=0;i<5;i++)MPI_Allreduce(MPI_IN_PLACE,b,n,MPI_FLOAT,MPI_SUM,MPI_COMM_WORLD);
t=MPI_Wtime()-t;if(!r)printf("{\"mpi_algBW_GBps\":%.3f}\n",16777216*5/t/1e9);MPI_Finalize();}
EOF
mpic++ -O3 /tmp/mpi_allreduce.c -o /tmp/mpi_allreduce && mpirun -np 4 --oversubscribe /tmp/mpi_allreduce
