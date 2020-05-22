/*
 * This file is part of a C++ interface to the Radiative Transfer for Energetics (RTE)
 * and Rapid Radiative Transfer Model for GCM applications Parallel (RRTMGP).
 *
 * The original code is found at https://github.com/RobertPincus/rte-rrtmgp.
 *
 * Contacts: Robert Pincus and Eli Mlawer
 * email: rrtmgp@aer.com
 *
 * Copyright 2015-2020,  Atmospheric and Environmental Research and
 * Regents of the University of Colorado.  All right reserved.
 *
 * This C++ interface can be downloaded from https://github.com/microhh/rte-rrtmgp-cpp
 *
 * Contact: Chiel van Heerwaarden
 * email: chiel.vanheerwaarden@wur.nl
 *
 * Copyright 2020, Wageningen University & Research.
 *
 * Use and duplication is permitted under the terms of the
 * BSD 3-clause license, see http://opensource.org/licenses/BSD-3-Clause
 *
 */

// #include <boost/algorithm/string.hpp>
// #include <cmath>

#include "Status.h"
#include "Netcdf_interface.h"
#include "Array.h"
#include "Gas_concs.h"
#include "Radiation_solver.h"

#ifdef FLOAT_SINGLE_RRTMGP
#define FLOAT_TYPE float
#else
#define FLOAT_TYPE double
#endif

template<typename TF>
void read_and_set_vmr(
        const std::string& gas_name, const int n_col, const int n_lay,
        const Netcdf_handle& input_nc, Gas_concs<TF>& gas_concs)
{
    const std::string vmr_gas_name = "vmr_" + gas_name;

    if (input_nc.variable_exists(vmr_gas_name))
    {
        std::map<std::string, int> dims = input_nc.get_variable_dimensions(vmr_gas_name);
        const int n_dims = dims.size();

        if (n_dims == 0)
        {
            gas_concs.set_vmr(gas_name, input_nc.get_variable<TF>(vmr_gas_name));
        }
        else if (n_dims == 1)
        {
            if (dims.at("lay") == n_lay)
                gas_concs.set_vmr(gas_name,
                                  Array<TF,1>(input_nc.get_variable<TF>(vmr_gas_name, {n_lay}), {n_lay}));
            else
                throw std::runtime_error("Illegal dimensions of gas \"" + gas_name + "\" in input");
        }
        else if (n_dims == 2)
        {
            if (dims.at("lay") == n_lay && dims.at("col") == n_col)
                gas_concs.set_vmr(gas_name,
                                  Array<TF,2>(input_nc.get_variable<TF>(vmr_gas_name, {n_lay, n_col}), {n_col, n_lay}));
            else
                throw std::runtime_error("Illegal dimensions of gas \"" + gas_name + "\" in input");
        }
    }
    else
    {
        Status::print_warning("Gas \"" + gas_name + "\" not available in input file.");
    }
}

template<typename TF>
void solve_radiation()
{
    Netcdf_file input_nc("rte_rrtmgp_input.nc", Netcdf_mode::Read);

    ////// READ THE ATMOSPHERIC DATA //////
    const int n_col = input_nc.get_dimension_size("col");
    const int n_lay = input_nc.get_dimension_size("lay");
    const int n_lev = input_nc.get_dimension_size("lev");
    const int n_bnd = input_nc.get_dimension_size("band");

    // Read the atmospheric fields.
    Array<TF,2> p_lay(input_nc.get_variable<TF>("lay"  , {n_lay, n_col}), {n_col, n_lay});
    Array<TF,2> t_lay(input_nc.get_variable<TF>("t_lay", {n_lay, n_col}), {n_col, n_lay});
    Array<TF,2> p_lev(input_nc.get_variable<TF>("lev"  , {n_lev, n_col}), {n_col, n_lev});
    Array<TF,2> t_lev(input_nc.get_variable<TF>("t_lev", {n_lev, n_col}), {n_col, n_lev});

    // Read the boundary conditions for longwave.
    Array<TF,2> emis_sfc(input_nc.get_variable<TF>("emis_sfc", {n_col, n_bnd}), {n_bnd, n_col});
    Array<TF,1> t_sfc(input_nc.get_variable<TF>("t_sfc", {n_col}), {n_col});

    Gas_concs<TF> gas_concs;
    read_and_set_vmr("h2o", n_col, n_lay, input_nc, gas_concs);
    read_and_set_vmr("co2", n_col, n_lay, input_nc, gas_concs);
    read_and_set_vmr("o3" , n_col, n_lay, input_nc, gas_concs);
    read_and_set_vmr("n2o", n_col, n_lay, input_nc, gas_concs);
    read_and_set_vmr("co" , n_col, n_lay, input_nc, gas_concs);
    read_and_set_vmr("ch4", n_col, n_lay, input_nc, gas_concs);
    read_and_set_vmr("o2" , n_col, n_lay, input_nc, gas_concs);
    read_and_set_vmr("n2" , n_col, n_lay, input_nc, gas_concs);

    // Fetch the col_dry in case present.
    Array<TF,2> col_dry({n_col, n_lay});
    if (input_nc.variable_exists("col_dry"))
        col_dry = input_nc.get_variable<TF>("col_dry", {n_lay, n_col});
    else
        Gas_optics_rrtmgp<TF>::get_col_dry(col_dry, gas_concs.get_vmr("h2o"), p_lev);


    ////// INITIALIZE THE SOLVER SO THE N_GPT CAN BE RETRIEVED //////
    Status::print_message("Initializing the solver.");
    Radiation_solver<TF> radiation(gas_concs);

    ////// CREATE THE OUTPUT ARRAYS THAT NEED TO BE STORED //////
    const int n_gpt = radiation.get_n_gpt();

    Array<TF,3> tau           ({n_col, n_lay, n_gpt});
    Array<TF,3> lay_source    ({n_col, n_lay, n_gpt});
    Array<TF,3> lev_source_inc({n_col, n_lay, n_gpt});
    Array<TF,3> lev_source_dec({n_col, n_lay, n_gpt});
    Array<TF,2> sfc_source    ({n_col, n_gpt});

    Array<TF,2> lw_flux_up ({n_col, n_lev});
    Array<TF,2> lw_flux_dn ({n_col, n_lev});
    Array<TF,2> lw_flux_net({n_col, n_lev});

    Array<TF,3> lw_bnd_flux_up ({n_col, n_lev, n_bnd});
    Array<TF,3> lw_bnd_flux_dn ({n_col, n_lev, n_bnd});
    Array<TF,3> lw_bnd_flux_net({n_col, n_lev, n_bnd});

    Status::print_message("Solving the longwave radiation.");
    radiation.solve_longwave(
            gas_concs,
            p_lay, p_lev,
            t_lay, t_lev,
            col_dry,
            t_sfc, emis_sfc,
            tau, lay_source, lev_source_inc, lev_source_dec, sfc_source,
            lw_flux_up, lw_flux_dn, lw_flux_net,
            lw_bnd_flux_up, lw_bnd_flux_dn, lw_bnd_flux_net);


    ////// SAVING THE MODEL OUTPUT //////
    Status::print_message("Saving the output to NetCDF.");

    Netcdf_file output_nc("rte_rrtmgp_output.nc", Netcdf_mode::Create);
    output_nc.add_dimension("col", n_col);
    output_nc.add_dimension("lay", n_lay);
    output_nc.add_dimension("lev", n_lev);
    output_nc.add_dimension("gpt", n_gpt);
    output_nc.add_dimension("band", n_bnd);
    output_nc.add_dimension("pair", 2);

    auto nc_lay = output_nc.add_variable<TF>("lay", {"lay"});
    auto nc_lev = output_nc.add_variable<TF>("lev", {"lev"});

    nc_lay.insert(p_lay.v(), {0});
    nc_lev.insert(p_lev.v(), {0});
    
    // WARNING: The storage in the NetCDF interface uses C-ordering and indexing.
    // First, store the optical properties.
    auto nc_band_lims_wvn = output_nc.add_variable<TF>("band_lims_wvn", {"band", "pair"});
    auto nc_band_lims_gpt = output_nc.add_variable<int>("band_lims_gpt", {"band", "pair"});

    nc_band_lims_wvn.insert(radiation.get_band_lims_wavenumber().v(), {0, 0});
    nc_band_lims_gpt.insert(radiation.get_band_lims_gpoint().v()    , {0, 0});

    auto nc_tau = output_nc.add_variable<TF>("tau", {"gpt", "lay", "col"});
    nc_tau.insert(tau.v(), {0, 0, 0});

    // Second, store the sources.
    auto nc_lay_src     = output_nc.add_variable<TF>("lay_src"    , {"gpt", "lay", "col"});
    auto nc_lev_src_inc = output_nc.add_variable<TF>("lev_src_inc", {"gpt", "lay", "col"});
    auto nc_lev_src_dec = output_nc.add_variable<TF>("lev_src_dec", {"gpt", "lay", "col"});

    auto nc_sfc_src = output_nc.add_variable<TF>("sfc_src", {"gpt", "col"});

    nc_lay_src.insert    (lay_source.v()    , {0, 0, 0});
    nc_lev_src_inc.insert(lev_source_inc.v(), {0, 0, 0});
    nc_lev_src_dec.insert(lev_source_dec.v(), {0, 0, 0});

    nc_sfc_src.insert(sfc_source.v(), {0, 0});

    // Save the output of the flux calculation to disk.
    auto nc_flux_up  = output_nc.add_variable<TF>("lw_flux_up" , {"lev", "col"});
    auto nc_flux_dn  = output_nc.add_variable<TF>("lw_flux_dn" , {"lev", "col"});
    auto nc_flux_net = output_nc.add_variable<TF>("lw_flux_net", {"lev", "col"});

    auto nc_bnd_flux_up  = output_nc.add_variable<TF>("lw_bnd_flux_up" , {"band", "lev", "col"});
    auto nc_bnd_flux_dn  = output_nc.add_variable<TF>("lw_bnd_flux_dn" , {"band", "lev", "col"});
    auto nc_bnd_flux_net = output_nc.add_variable<TF>("lw_bnd_flux_net", {"band", "lev", "col"});

    nc_flux_up .insert(lw_flux_up .v(), {0, 0});
    nc_flux_dn .insert(lw_flux_dn .v(), {0, 0});
    nc_flux_net.insert(lw_flux_net.v(), {0, 0});

    nc_bnd_flux_up .insert(lw_bnd_flux_up .v(), {0, 0, 0});
    nc_bnd_flux_dn .insert(lw_bnd_flux_dn .v(), {0, 0, 0});
    nc_bnd_flux_net.insert(lw_bnd_flux_net.v(), {0, 0, 0});
}

int main()
{
    try
    {
        solve_radiation<FLOAT_TYPE>();
    }

    // Catch any exceptions and return 1.
    catch (const std::exception& e)
    {
        std::string error = "EXCEPTION: " + std::string(e.what());
        Status::print_message(error);
        return 1;
    }
    catch (...)
    {
        Status::print_message("UNHANDLED EXCEPTION!");
        return 1;
    }

    // Return 0 in case of normal exit.
    return 0;
}
