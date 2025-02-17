python3 configure.py \
 --prob=MultiPhase \
 --nghost=2 \
 --eos="isothermal" \
 -b \
 -fft \
 -hdf5 \
 -h5double \
 -mpi \
 --mpiccmd=h5pcc \
 --cflag='-DH5_HAVE_PARALLEL -lstdc++' \
 --cxx='g++'
