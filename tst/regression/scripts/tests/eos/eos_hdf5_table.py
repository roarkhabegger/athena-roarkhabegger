"""
Regression test for general EOS 1D Sod shock tube.
"""

# Modules
import numpy as np                             # standard Python module for numerics
import sys                                     # standard Python module to change path
import os
from shutil import move                        # moves/renames files
import scripts.utils.athena as athena          # utilities for running Athena++
import scripts.utils.comparison as comparison  # more utilities explicitly for testing
sys.path.insert(0, '../../vis/python')         # insert path to Python read scripts
from .eos_table_test import mk_ideal, write_H
import athena_read                             # utilities for reading Athena++ data

_gammas = [1.1, 1.4, 5./3.]


def prepare(**kwargs):
    """
    Configure and make the executable.

    This function is called first. It is responsible for calling the configure script and
    make to create an executable. It takes no inputs and produces no outputs.
    """

    athena.configure('hdf5',
                     prob='shock_tube',
                     coord='cartesian',
                     flux='hllc',
                     eos='eos_table',
                     **kwargs)
    athena.make()
    src = os.path.join('bin', 'athena')
    dst = os.path.join('bin', 'athena_eos_hllc')
    move(src, dst)

    athena.configure(
                     prob='shock_tube',
                     coord='cartesian',
                     flux='hllc',
                     eos='hydrogen',
                     **kwargs)
    athena.make()
    src = os.path.join('bin', 'athena')
    dst = os.path.join('bin', 'athena_H')
    move(src, dst)

    athena.configure(
                     prob='shock_tube',
                     coord='cartesian',
                     flux='hllc',
                     eos='adiabatic',
                     **kwargs)
    athena.make()

    write_H(binary=False, ascii=False, hdf5=True)
    for g in _gammas:
        mk_ideal(g, out_type='hdf5')


def run(**kwargs):
    """``
    Run the executable.

    This function is called second. It is responsible for calling the Athena++ binary in
    such a way as to produce testable output. It takes no inputs and produces no outputs.
    """

    arguments0 = ['hydro/gamma={0:}', 'job/problem_id=Sod_ideal_{1:}', 'time/ncycle_out=0',
                  'output1/file_type=vtk']
    for i, g in enumerate(_gammas):
        arguments = [j.format(g, i) for j in arguments0]
        athena.run('hydro/athinput.sod', arguments)

    src = os.path.join('bin', 'athena_eos_hllc')
    dst = os.path.join('bin', 'athena')
    move(src, dst)
    arguments0[1] = 'job/problem_id=Sod_eos_hllc_{1:}'
    arguments0.extend(
        ['hydro/EOS_file_name=gamma_is_{0:.3f}.hdf5', 'hydro/EOS_file_type=hdf5'])
    for i, g in enumerate(_gammas):
        arguments = [j.format(g, i) for j in arguments0]
        athena.run('hydro/athinput.sod', arguments)
    # now run with simple H table
    arguments0[0] = 'hydro/gamma=1.6667'
    arguments0[1] = 'job/problem_id=Sod_eos_H_hdf5'
    arguments0[-2] = 'hydro/EOS_file_name=SimpleHydrogen.hdf5'
    tmp = ['dl', 'ul', 'pl', 'dr', 'ur', 'pr']
    tmp = ['problem/' + i + '={0:}'for i in tmp] + ['time/tlim={0:}']
    tmp = zip(tmp, [1e-07, 0.00, 3e-8, 1.25e-8, 0.00, 1e-9, .25])
    ic = [i[0].format(i[1]) for i in tmp]
    athena.run('hydro/athinput.sod', ic + arguments0)

    src = os.path.join('bin', 'athena_H')
    dst = os.path.join('bin', 'athena')
    move(src, dst)
    arguments0[1] = 'job/problem_id=Sod_eos_H'
    athena.run('hydro/athinput.sod', ic + arguments0[:-2])

def analyze():
    """
    Analyze the output and determine if the test passes.

    This function is called third; nothing from this file is called after it. It is
    responsible for reading whatever data it needs and making a judgment about whether or
    not the test passes. It takes no inputs. Output should be True (test passes) or False
    (test fails).
    """

    analyze_status = True
    for i, g in enumerate(_gammas):
        for t in [10, 26]:
            x_ref, _, _, data_ref = athena_read.vtk(
                'bin/Sod_ideal_{0:}.block0.out1.{1:05d}.vtk'.format(i, t))
            x_new, _, _, data_new = athena_read.vtk(
                'bin/Sod_eos_hllc_{0:}.block0.out1.{1:05d}.vtk'.format(i, t))
            loc = [0, 0, slice(None)]
            for var in ['rho', 'press']:
                diff = comparison.l1_diff(
                    x_ref, data_ref[var][loc], x_new, data_new[var][loc])
                diff /= comparison.l1_norm(x_ref, data_ref[var][loc])
                if diff > 1e-3 or np.isnan(diff):
                    print(
                        ' '.join(map(str, ['Eos hdf5 table fail. var, diff, gamma =', var, diff, g])))
                    analyze_status = False

    tol = [3e-3, 6e-4]
    for i,t in enumerate([10, 26]):
        x_ref, _, _, data_ref = athena_read.vtk(
            'bin/Sod_eos_H.block0.out1.{1:05d}.vtk'.format(i, t))
        x_new, _, _, data_new = athena_read.vtk(
            'bin/Sod_eos_H_hdf5.block0.out1.{1:05d}.vtk'.format(i, t))
        loc = [0, 0, slice(None)]
        for var in ['rho', 'press']:
            norm = comparison.l1_norm(x_ref, data_ref[var][loc])
            diff = comparison.l1_diff(
                x_ref, data_ref[var][loc], x_new, data_new[var][loc]) / norm
            if diff > tol[i] or np.isnan(diff):
                print(
                    ' '.join(map(str, ['Eos H table test fail. var, err =', var, diff])))
                analyze_status = False

    return analyze_status
