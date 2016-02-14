#include "water_balance.h"


void calculate_water_balance(control *c, fluxes *f, met *m, params *p,
                             state *s, int day_idx, int daylen,
                             double trans_leaf) {
    /*

    Calculate the water balance (including all water fluxes).

    Parameters:
    ----------
    control : structure
        control structure
    fluxes : structure
        fluxes structure
    met : structure
        meteorological drivers structure
    params : structure
        parameters structure
    day : int
        project day. (Dummy argument, only passed for daily model)
    daylen : double
        length of day in hours. (Dummy argument, only passed for daily model)
    trans_leaf : double
        leaf transpiration (Dummy argument, only passed for sub-daily model)

    */
    double soil_evap, et, interception, runoff, rain, press, sw_rad, conv,
           tair, transpiration;

    double net_rad_day, net_rad_am, net_rad_pm, trans_am, omega_am,
           gs_mol_m2_hfday_am, ga_mol_m2_hfday_am, tair_am, tair_pm, tair_day,
           sw_rad_am, sw_rad_pm, sw_rad_day, vpd_am, vpd_pm, vpd_day,
           wind_am, wind_pm, wind_day, ca, gpp_am, gpp_pm, trans_pm,
           omega_pm, gs_mol_m2_hfday_pm, ga_mol_m2_hfday_pm, DAY_2_SEC;

    /* unpack met forcing */
    if (c->sub_daily) {
        rain = m->rain[c->hrly_idx];
        press = m->press[c->hrly_idx] * KPA_2_PA;
        tair = m->tair[c->hrly_idx];
        sw_rad = m->par[c->hrly_idx] * PAR_2_SW; /* W m-2 */
    } else {
        ca = m->co2[day_idx];
        tair = m->tair[day_idx];
        tair_am = m->tam[day_idx];
        tair_pm = m->tpm[day_idx];
        sw_rad = m->sw_rad[day_idx];
        sw_rad_am = m->sw_rad_am[day_idx];
        sw_rad_pm = m->sw_rad_pm[day_idx];
        rain = m->rain[day_idx];
        vpd_day = m->vpd_avg[day_idx];
        vpd_am = m->vpd_am[day_idx];
        vpd_pm = m->vpd_pm[day_idx];
        wind_am = m->wind_am[day_idx];
        wind_pm = m->wind_pm[day_idx];
        wind_day = m->wind[day_idx];
        press = m->press[day_idx] * KPA_2_PA;
    }

    interception = calc_infiltration(p, s, rain);
    soil_evap = calc_soil_evaporation(p, s, sw_rad, press, tair);
    if (c->sub_daily) {
        /*
            W/m2 = 1000 (kg/m3) * 2.501 * 10^6 (J/kg) * 1 mm/day * \
                    (1/1800.0) (30 min/s) * (1/1000) (mm/30min)

                 = 1.0 / 1389.44 mm/30 min
        */
        /*soil_evap /= 1389.44;*/

        soil_evap *= MOLE_WATER_2_G_WATER * G_TO_KG * SEC_2_HLFHR;
    } else {
        /*
            W/m2 = 1000 (kg/m3) * 2.501 * 10^6 (J/kg) * 1 mm/day * \
                    (1/86400.0) (day/s) * (1/1000) (mm/day)
        */
        /*conv = 1000.0 * 2.501 * 1E6 * \
                 (1. / (60.0 * 60.0 * daylen)) * (1.0 / 1000.);
        soil_evap /= conv;*/
        soil_evap *= MOLE_WATER_2_G_WATER * G_TO_KG * (60.0 * 60.0 * daylen);
    }


    if (c->sub_daily) {

        /* mol m-2 s-1 to mm/30 min */
        transpiration = trans_leaf * MOLE_WATER_2_G_WATER * G_TO_KG * \
                        SEC_2_HLFHR;

    } else {
        /* local vars for readability */
        gpp_am = f->gpp_am;
        gpp_pm = f->gpp_pm;

        calc_transpiration_penmon_am_pm(p, s, net_rad_am, wind_am, ca, daylen,
                                        press, vpd_am, tair_am, gpp_am,
                                        &trans_am, &omega_am,
                                        &gs_mol_m2_hfday_am,
                                        &ga_mol_m2_hfday_am);

        calc_transpiration_penmon_am_pm(p, s, net_rad_pm, wind_pm, ca, daylen,
                                        press, vpd_pm, tair_pm, gpp_pm,
                                        &trans_pm, &omega_pm,
                                        &gs_mol_m2_hfday_pm,
                                        &ga_mol_m2_hfday_pm);

        /* Unit conversions... */
        DAY_2_SEC = 1.0 / (60.0 * 60.0 * daylen);
        f->omega = (omega_am + omega_pm) / 2.0;

        /* output in mol H20 m-2 s-1 */
        f->gs_mol_m2_sec = ((gs_mol_m2_hfday_am +
                             gs_mol_m2_hfday_pm) * DAY_2_SEC);
        f->ga_mol_m2_sec = ((ga_mol_m2_hfday_am +
                             ga_mol_m2_hfday_pm) * DAY_2_SEC);

        /* mm day-1 */
        transpiration = trans_am + trans_pm;


    }


    et = transpiration + soil_evap + interception;
    update_water_storage(c, f, p, s, rain, interception, &transpiration,
                         &soil_evap, &et, &runoff);


    if (c->sub_daily) {
        sum_hourly_water_fluxes(f, soil_evap, transpiration, et,
                                interception, runoff);
    } else {
        update_daily_water_struct(f, soil_evap, transpiration, et,
                                  interception, runoff);

    }

    return;
}

void update_water_storage(control *c, fluxes *f, params *p, state *s,
                          double rain, double interception,
                          double *transpiration, double *soil_evap,
                          double *et, double *runoff) {
    /* Calculate root and top soil plant available water and runoff.

    Soil drainage is estimated using a "leaky-bucket" approach with two
    soil layers. In reality this is a combined drainage and runoff
    calculation, i.e. "outflow". There is no drainage out of the "bucket"
    soil.

    Returns:
    --------
    outflow : float
        outflow [mm d-1]
    */
    double trans_frac, previous;

    /* reduce transpiration from the top soil if it is dry */
    trans_frac = p->fractup_soil * s->wtfac_topsoil;

    /* Total soil layer */
    s->pawater_topsoil += (rain - interception) - \
                          (*transpiration * trans_frac) - \
                           *soil_evap;

    if (s->pawater_topsoil < 0.0) {
        s->pawater_topsoil = 0.0;
    } else if (s->pawater_topsoil > p->wcapac_topsoil) {
        s->pawater_topsoil = p->wcapac_topsoil;
    }

    /* Total root zone */
    previous = s->pawater_root;
    s->pawater_root += (rain - interception) - *transpiration - *soil_evap;

    /* calculate runoff and remove any excess from rootzone */
    if (s->pawater_root > p->wcapac_root) {
        *runoff = s->pawater_root - p->wcapac_root;
        s->pawater_root -= *runoff;
    } else {
        *runoff = 0.0;
    }

    if (s->pawater_root < 0.0) {
        *transpiration = 0.0;
        *soil_evap = 0.0;
        *et = interception;
    }

    if (s->pawater_root < 0.0)
        s->pawater_root = 0.0;
    else if (s->pawater_root > p->wcapac_root)
        s->pawater_root = p->wcapac_root;

    s->delta_sw_store = s->pawater_root - previous;

    if (c->water_stress) {
        /* Calculate the soil moisture availability factors [0,1] in the
           topsoil and the entire root zone */
        calculate_soil_water_fac(c, p, s);
    } else {
        /* really this should only be a debugging option! */
        s->wtfac_topsoil = 1.0;
        s->wtfac_root = 1.0;
    }

    return;
}



double calc_infiltration(params *p, state* s, double rain) {
    /* Estimate "effective" rain, or infiltration I guess.

    Simple assumption that infiltration relates to leaf area
    and therefore canopy storage capacity (wetloss). Interception is
    likely to be ("more") erroneous if a canopy is subject to frequent daily
    rainfall I would suggest.

    Parameters:
    -------
    rain : float
        rainfall [mm d-1]

    */
    double interception;

    if (s->lai > 0.0) {
        interception = (rain * p->intercep_frac * \
                        MIN(1.0, s->lai / p->max_intercep_lai));
    } else {
        interception = 0.0;
    }

    return (interception);
}

double calc_soil_evaporation(params *p, state *s, double sw_rad, double press,
                             double tair) {
    /* Use Penman eqn to calculate top soil evaporation flux at the
    potential rate.

    Soil evaporation is dependent upon soil wetness and plant cover. The net
    radiation term is scaled for the canopy cover passed to this func and
    the impact of soil wetness is accounted for in the wtfac term. As the
    soil dries the evaporation component reduces significantly.

    Key assumptions from Ritchie...

    * When plant provides shade for the soil surface, evaporation will not
    be the same as bare soil evaporation. Wind speed, net radiation and VPD
    will all belowered in proportion to the canopy density. Following
    Ritchie role ofwind, VPD are assumed to be negligible and are therefore
    ignored.

    These assumptions are based on work with crops and whether this holds
    for tree shading where the height from the soil to the base of the
    crown is larger is questionable.

    units = (mm/day)

    References:
    -----------
    * Ritchie, 1972, Water Resources Research, 8, 1204-1213.

    Parameters:
    -----------
    tair : float
        temperature [degC]
    sw_rad : float
        shortwave radiation [W m-2]
    press : float
        air pressure [kPa]

    Returns:
    --------
    soil_evap : float
        soil evaporation [mm d-1]

    */
    double lambda, gamma, slope, arg1, arg2, soil_evap, net_lw, net_rad;

    /* Latent heat of water vapour at air temperature (J mol-1) */
    lambda = (H2OLV0 - 2.365E3 * tair) * H2OMW;

    /* psychrometric constant */
    gamma = CP * MASS_AIR * press / lambda;

    /* Const s in Penman-Monteith equation  (Pa K-1) */
    arg1 = calc_sat_water_vapour_press(tair + 0.1);
    arg2 = calc_sat_water_vapour_press(tair);
    slope = (arg1 - arg2) / 0.1;

    /* Net loss of long-wave radn, Monteith & Unsworth '90, pg 52, eqn 4.17 */
    net_lw = 107.0 - 0.3 * tair;            /* W m-2 */

    /* Net radiation recieved by a surf, Monteith & Unsw '90, pg 54 eqn 4.21
        - note the minus net_lw is correct as eqn 4.17 is reversed in
          eqn 4.21, i.e Lu-Ld vs. Ld-Lu
        - NB: this formula only really holds for cloudless skies!
        - Bounding to zero, as we can't have negative soil evaporation, but you
          can have negative net radiation.
        - units: W m-2
    */
    net_rad = MAX(0.0, (1.0 - p->albedo) * sw_rad - net_lw);

    /* mol H20 m-2 s-1 */
    soil_evap = ((slope / (slope + gamma)) * net_rad) / lambda;

    /*
      Surface radiation is reduced by overstory LAI cover. This empirical
      fit comes from Ritchie (1972) and is formed by a fit between the LAI
      of 5 crops types and the fraction of observed net radiation at the
      surface. Whilst the LAI does cover a large range, nominal 0–6, there
      are only 12 measurements and only three from LAI > 3. So this might
      not hold as well for a forest canopy?
      Ritchie 1972, Water Resources Research, 8, 1204-1213.
    */
    if (s->lai > 0.0)
        soil_evap *= exp(-0.398 * s->lai);

    /* reduce soil evaporation if top soil is dry */
    soil_evap *= s->wtfac_topsoil;

    return (soil_evap);
}

void penman_leaf(params *p, state *s, double press, double vpd,
                 double tair, double tleaf, double wind, double rnet,
                 double gsc, double *transpiration, double *LE, double *gbc,
                 double *gh, double *gv) {
    /*
        Calculates transpiration by leaves using the Penman-Monteith

        Parameters:
        ----------
        press : float
            atmospheric pressure (Pa)
        rnet : float
            net radiation (J m-2 s-1)
        vpd : float
            vapour pressure deficit of air (Pa)
        tair : float
            air temperature (deg C)
        transpiration : float
            transpiration (mol H2O m-2 s-1) (returned)
        LE : float
            latent heat flux, W m-2 (returned)


    */
    double slope, omega, epsilon, lambda, arg1, arg2, gradn, gbhu, gbhf, gbh,
           gbv, gsv, gamma, Tdiff, sensible_heat, ema, Tk;

    /* Radiation conductance (mol m-2 s-1) */
    gradn = calc_radiation_conductance(tair);

    /* Boundary layer conductance for heat - single sided, forced
       convection (mol m-2 s-1) */
    gbhu = calc_bdn_layer_forced_conduct(tair, press, wind, p->leaf_width);

    /* Boundary layer conductance for heat - single sided, free convection */
    gbhf = calc_bdn_layer_free_conduct(tair, tleaf, press, p->leaf_width);

    /* Total boundary layer conductance for heat */
    gbh = gbhu + gbhf;

    /* Total conductance for heat - two-sided */
    *gh = 2.0 * (gbh + gradn);

    /* Total conductance for water vapour */
    gbv = GBVGBH * gbh;
    gsv = GSVGSC * gsc;
    *gv = (gbv * gsv) / (gbv + gsv);
    *gbc = gbh / GBHGBC;

    /* Latent heat of water vapour at air temperature (J mol-1) */
    lambda = (H2OLV0 - 2.365E3 * tair) * H2OMW;

    /* psychrometric constant */
    gamma = CP * MASS_AIR * press / lambda;

    /* Const s in Penman-Monteith equation  (Pa K-1) */
    arg1 = calc_sat_water_vapour_press(tair + 0.1);
    arg2 = calc_sat_water_vapour_press(tair);
    slope = (arg1 - arg2) / 0.1;

    if (*gv > 0.0) {
        arg1 = slope * rnet + vpd * *gh * CP * MASS_AIR;
        arg2 = slope + gamma * *gh / *gv;
        *LE = arg1 / arg2; /* W m-2 */
        *transpiration = *LE / lambda; /* mol H20 m-2 s-1 */
    } else {
        *transpiration = 0.0;
    }

    /* Should not be negative - not sure gv>0.0 catches it as g0 = 1E-09? */
    *transpiration = MAX(0.0, *transpiration);

    /*
    ** Move to canopy as it makes little sense here at the leaf scale...
    */

    /* Calculate decoupling coefficient (McNaughton and Jarvis 1986) */
    epsilon = slope / gamma;
    omega = (1.0 + epsilon) / (1.0 + epsilon + gbv / gsv);

    return;
}


double calc_sat_water_vapour_press(double tac) {
    /*
        Calculate saturated water vapour pressure (Pa) at
        temperature TAC (Celsius). From Jones 1992 p 110 (note error in
        a - wrong units)
    */
    return (613.75 * exp(17.502 * tac / (240.97 + tac)));
}









void calc_transpiration_penmon(fluxes *f, params *p, state *s, double vpd, double net_rad,
                               double tavg, double wind, double ca,
                               double daylen, double press) {
    /* Calculate canopy transpiration using the Penman-Monteith equation.
    units mm/day

    Parameters:
    -----------
    vpd : float
        average daily vpd [kPa]
    net_rad : float
        net radiation [mj m-2 s-1]
    tavg : float
        average daytime temp [degC]
    wind : float
        average daily wind speed [m s-1]
    ca : float
        atmospheric co2 [umol mol-1]
    daylen : float
        daylength in hours
    press : float
        average daytime pressure [kPa]

    */
    double SEC_2_DAY, DAY_2_SEC, gs_mol_m2_sec, tk, MOL_SEC_2_M_PER_SEC,
           trans, omegax, M_PER_SEC_2_MOL_SEC, gs_m_per_sec,
           ga_m_per_sec;

    SEC_2_DAY =  60.0 * 60.0 * daylen;
    DAY_2_SEC = 1.0 / SEC_2_DAY;
    gs_mol_m2_sec = calc_stomatal_conductance(p->g1, s->wtfac_root, vpd, ca, daylen,
                                              f->gpp_gCm2);


    /* convert units
        - mol/sec to m/s See Jones, 1992, appendix */
    tk = tavg + DEG_TO_KELVIN;
    MOL_SEC_2_M_PER_SEC = MM_TO_M / (press / (RGAS * tk));
    M_PER_SEC_2_MOL_SEC = 1.0 / MOL_SEC_2_M_PER_SEC;

    gs_m_per_sec = gs_mol_m2_sec * MOL_SEC_2_M_PER_SEC;
    ga_m_per_sec = canopy_boundary_layer_conductance(p, wind, s->canht);

    penman_monteith(vpd, gs_m_per_sec, net_rad, tavg, press,
                    ga_m_per_sec, &omegax, &trans);

    f->gs_mol_m2_sec = gs_mol_m2_sec;
    f->ga_mol_m2_sec = ga_m_per_sec * M_PER_SEC_2_MOL_SEC;
    f->transpiration = trans * SEC_2_DAY;

    return;
}


void calc_transpiration_penmon_am_pm(params *p, state *s, double net_rad,
                                     double wind, double ca, double daylen,
                                     double press, double vpd, double tair,
                                     double gpp, double *trans, double *omega,
                                     double *gs_mol_m2_hfday,
                                     double *ga_mol_m2_hfday) {
    /* Calculate canopy transpiration using the Penman-Monteith equation
    using am and pm data [mm/day]

    Parameters:
    -----------
    vpd : float
        average daily vpd [kPa]
    net_rad_am : float
        net radiation [mj m-2 s-1] (morning)
    net_rad_pm : float
        net radiation [mj m-2 s-1] (afternoon)
    tair : float
        AM/PM air temp [degC] am/pm
    wind : float
        daily wind speed [m s-1]
    ca : float
        atmospheric co2 [umol mol-1]
    daylen : float
        daylength in hours
    press : float
        average daytime pressure [kPa]
    */
    double half_day, SEC_2_HALF_DAY, HALF_DAY_2_SEC, Tk, MOL_SEC_2_M_PER_SEC,
           M_PER_SEC_2_MOL_SEC, gs_mol_m2_sec, ga_m_per_sec,
           gs_m_per_sec;

    half_day = daylen / 2.0;

    /* time unit conversions */
    SEC_2_HALF_DAY =  60.0 * 60.0 * half_day;
    HALF_DAY_2_SEC = 1.0 / SEC_2_HALF_DAY;

    Tk = tair + DEG_TO_KELVIN;
    MOL_SEC_2_M_PER_SEC = MM_TO_M / (press / (RGAS * Tk));
    M_PER_SEC_2_MOL_SEC = 1.0 / MOL_SEC_2_M_PER_SEC;

    ga_m_per_sec = canopy_boundary_layer_conductance(p, wind, s->canht);
    gs_mol_m2_sec = calc_stomatal_conductance(p->g1, s->wtfac_root, vpd, ca,
                                              half_day, gpp);

    /* unit conversions */
    *ga_mol_m2_hfday = ga_m_per_sec * M_PER_SEC_2_MOL_SEC * SEC_2_HALF_DAY;
    *gs_mol_m2_hfday = gs_mol_m2_sec * SEC_2_HALF_DAY;
    gs_m_per_sec = gs_mol_m2_sec * MOL_SEC_2_M_PER_SEC;

    penman_monteith(vpd, gs_m_per_sec, net_rad, tair, press, ga_m_per_sec,
                    *(&omega), *(&trans));

    /* convert to mm/half day */
    *trans *= SEC_2_HALF_DAY;

    return;
}

double calc_stomatal_conductance(double g1, double wtfac, double vpd, double ca,
                                 double daylen, double gpp) {
    /* Calculate stomatal conductance, note assimilation rate has been
    adjusted for water availability at this point.

    gs = g0 + 1.6 * (1 + g1/sqrt(D)) * A / Ca

    units: m s-1 (conductance H2O)
    References:
    -----------
    For conversion factor for conductance see...
    * Jones (1992) Plants and microclimate, pg 56 + Appendix 3
    * Diaz et al (2007) Forest Ecology and Management, 244, 32-40.

    Stomatal Model:
    * Medlyn et al. (2011) Global Change Biology, 17, 2134-2144.
    **Note** Corrigendum -> Global Change Biology, 18, 3476.

    Parameters:
    -----------
    g1 : float
        slope
    wtfac : float
        water availability scaler [0,1]
    vpd : float
        average daily vpd [kPa]
    ca : float
        atmospheric co2 [umol mol-1]
    daylen : float
        daylength in hours

    Returns:
    --------
    gs : float
        stomatal conductance [mol m-2 s-1]
    */
    double DAY_2_SEC, gpp_umol_m2_sec, arg1, arg2;

    DAY_2_SEC = 1.0 / (60.0 * 60.0 * daylen);
    gpp_umol_m2_sec = gpp * GRAMS_C_TO_MOL_C * MOL_TO_UMOL * DAY_2_SEC;

    arg1 = 1.6 * (1.0 + (g1 * wtfac) / sqrt(vpd));
    arg2 = gpp_umol_m2_sec / ca;

    /* mol m-2 s-1 */
    return (arg1 * arg2);

}


double canopy_boundary_layer_conductance(params *p, double wind, double canht) {
    /*  Canopy boundary layer conductance, ga or 1/ra

    Characterises the heat/water vapour from evaporating surface, but does
    not account for leaf boundary layer conductance, which is the parellel
    sum of single leaf boundary layer conductances for all leaves in the
    canopy.

    Notes:
    ------
    'Estimates of ga for pine canopies from LAI of 3 to 6 vary from
    3.5 to 1.1 mol m-2 s-1  (Kelliher et al., 1993; Juang et al., 2007).'
    Drake et al, 2010, 17, pg. 1526.

    References:
    ------------
    * Jones 1992, pg. 67-8.
    * Monteith and Unsworth (1990), pg. 248. Note this in the inverted form
      of what is in Monteith (ga = 1 / ra)
    * Allen et al. (1989) pg. 651.
    * Gash et al. (1999) Ag forest met, 94, 149-158.

    Parameters:
    -----------
    wind : float
        average daytime wind speed [m s-1]

    Returns:
    --------
    ga : float
        canopy boundary layer conductance [m s-1]
    */

    /* z0m roughness length governing momentum transfer [m] */
    double z0m, z0h, d, arg1, arg2, arg3;
    double vk = 0.41;
    z0m = p->dz0v_dh * canht;

    /*
       z0h roughness length governing transfer of heat and vapour [m]
      *Heat tranfer typically less efficent than momentum transfer. There is
       a lot of variability in values quoted for the ratio of these two...
       JULES uses 0.1, Campbell and Norman '98 say z0h = z0m / 5. Garratt
       and Hicks, 1973/ Stewart et al '94 say z0h = z0m / 7. Therefore for
       the default I am following Monteith and Unsworth, by setting the
       ratio to be 1, the code below is identical to that on page 249,
       eqn 15.7
    */
    z0h = p->z0h_z0m * z0m;

    /* zero plan displacement height [m] */
    d = p->displace_ratio * canht;

    arg1 = (vk * vk) * wind;
    arg2 = log((canht - d) / z0m);
    arg3 = log((canht - d) / z0h);

    return (arg1 / (arg2 * arg3));
}

void penman_monteith(double vpd, double gs, double net_rad, double tavg,
                     double press, double ga, double *omega,
                     double *et) {

    /* Water loss from a canopy (ET), representing surface as a big "leaf".
    The resistance to vapour transfer from the canopy to the atmosphere is
    determined by the aerodynamic canopy conductance (ga) and the stomatal
    conductance (gs). If the surface is wet then there is a further water vapour
    flux from the soil/surface (calculated elsewhere!).

    Assumption is that calculation is for the entire stand (one surface), e.g.
    the single-layer approach. Second major assumption is that soil heat is
    zero over the course of a day and is thus ignored.

    Value for cp comes from Allen et al 1998.

    units: mm day-1

    References:
    -----------
    * Monteith and Unsworth (1990) Principles of Environmental
      Physics, pg. 247. Although I have removed the soil heat flux as G'DAY
      calculates soil evaporation seperately.
    * Allen et al. (1989) Operational estimates of reference evapotranspiration.
      Agronomy Journal, 81, 650-662.
    * Allen et al. (1998) Crop evapotranspiration - Guidelines for computing
      crop water requirements - FAO Irrigation and drainage paper 56.
      http://www.fao.org/docrep/X0490E/x0490e00.htm#Contents. PDF in bibtex lib.
    * Harrison (1963) Fundamentals concepts and definitions relating to
      humidity. In Wexler, A. (Ed.) Humidity and moisture. Vol 3, Reinhold
      Publishing Co., New York, NY, USA.
    * Dawes and Zhang (2011) Waves - An integrated energy and water balance
      model http://www.clw.csiro.au/products/waves/downloads/chap3.pdf

    Parameters:
    -----------
    vpd : float
        vapour pressure def [kPa]
    wind : float
        average daytime wind speed [m s-1]
    gs : float
        stomatal conductance [m s-1]
    net_rad : float
        net radiation [mj m-2 s-1]
    tavg : float
        daytime average temperature [degC]
    press : float
        average daytime pressure [kPa]

    Returns:
    --------
    et : float
        evapotranspiration [mm d-1]

    */

    /*
    if press == None:
        press = self.calc_atmos_pressure()
    */
    double lambdax, gamma, slope, rho, e, arg1, arg2;
    double cp = 1.013E-3;

    lambdax = calc_latent_heat_of_vapourisation(tavg);
    gamma = calc_pyschrometric_constant(lambdax, press);
    slope = calc_slope_of_saturation_vapour_pressure_curve(tavg);
    rho = calc_density_of_air(tavg);

    if (gs > 0.0) {
        /* decoupling coefficent, Jarvis and McNaughton, 1986
           when omega is close to zero, it is said to be well coupled and
           gs is the dominant controller of water loss (gs<ga). */
        e = slope / gamma; /* chg of latent heat relative to sensible heat of air */
        *omega = (e + 1.0) / (e + 1.0 + (ga / gs));

        arg1 = ((slope * net_rad ) + (rho * cp * vpd * ga));
        arg2 = slope + gamma * (1.0 + (ga / gs));
        *et = (arg1 / arg2) / lambdax;
    } else {
        *et = 0.0;
        *omega = 0.0;
    }
    return;
}


double calc_slope_of_saturation_vapour_pressure_curve(double tavg) {
    /* Eqn 13 from FAO paper, Allen et al. 1998.

    Parameters:
    -----------
    tavg : float
        average daytime temperature

    Returns:
    --------
    slope : float
        slope of saturation vapour pressure curve [kPa degC-1]

    */
    double t, arg1, arg2;

    t = tavg + 237.3;
    arg1 = 4098.0 * (0.6108 * exp((17.27 * tavg) / t));
    arg2 = t * t;
    return (arg1 / arg2);
}

double calc_pyschrometric_constant(double lambdax, double press) {
    /* Psychrometric constant ratio of specific heat of moist air at
    a constant pressure to latent heat of vaporisation.

    References:
    -----------
    * Eqn 8 from FAO paper, Allen et al. 1998.

    Parameters:
    -----------
    lambdax : float
         latent heat of water vaporization [MJ kg-1]
    press : float
        average daytime pressure [kPa]

    Returns:
    --------
    gamma : float
        pyschrometric_constant [kPa degC-1]

    */
    double cp = 1.013E-3;
    double epsilon = 0.6222;

    return ((cp * press) / (epsilon * lambdax));
}

double calc_atmos_pressure() {
    /* Pressure exerted by the weight of earth's atmosphere.

    References:
    -----------
    * Eqn 7 from FAO paper, Allen et al. 1998.

    Returns:
    --------
    press : float
        modelled average daytime pressure [kPa]

    */
    double zele_sea = 125.0;
    return (101.3 * pow(((293.0 - 0.0065 * zele_sea) / (293.0)),5.26));
}

double calc_latent_heat_of_vapourisation(double tavg) {
    /*  After Harrison (1963), should roughly = 2.45 MJ kg-1

        Returns:
        -----------
        lambdax : float
            latent heat of water vaporization [MJ kg-1]
    */
    return (2.501 - 0.002361 * tavg);
}

double calc_density_of_air(double tavg) {
    /*  Found in lots of places but only reference I could find it in that
        wasn't an uncited equation is Dawes and Zhang (2011). No doubt there
        is a better reference

        Parameters:
        -----------
        tavg : float
            average daytime temperature [degC]

        Returns:
        --------
        density : float
            density of air [kg m-3]
    */
    return (1.292 - (0.00428 * tavg));
}






void initialise_soil_moisture_parameters(control *c, params *p) {
    /*
      initialise parameters, if these are not known for the site use
      values derived from Cosby et al to calculate the amount of plant
      available water.
     */

    double theta_fc_topsoil, theta_wp_topsoil, theta_fc_root, theta_wp_root;
    double *fsoil_top = NULL, *fsoil_root = NULL;

    if (c->calc_sw_params) {
        fsoil_top = get_soil_fracs(p->topsoil_type);
        fsoil_root = get_soil_fracs(p->rootsoil_type);

        /* top soil */
        calc_soil_params(fsoil_top, &theta_fc_topsoil, &theta_wp_topsoil,
                         &p->theta_sat_topsoil, &p->b_topsoil, &p->psi_sat_topsoil);

        /* Plant available water in top soil (mm) */
        p->wcapac_topsoil = p->topsoil_depth * (theta_fc_topsoil - theta_wp_topsoil);

        /* Root zone */
        calc_soil_params(fsoil_root, &theta_fc_root, &theta_wp_root,
                         &p->theta_sat_root, &p->b_root, &p->psi_sat_root);

        /* Plant available water in rooting zone (mm) */
        p->wcapac_root = p->rooting_depth * (theta_fc_root - theta_wp_root);
    }

    /* calculate Landsberg and Waring SW modifier parameters if not
       specified by the user based on a site calibration */
    if (p->ctheta_topsoil < -900.0 && p->ntheta_topsoil  < -900.0 &&
        p->ctheta_root < -900.0 && p->ntheta_root < -900.0) {
        get_soil_params(p->topsoil_type, &p->ctheta_topsoil, &p->ntheta_topsoil);
        get_soil_params(p->rootsoil_type, &p->ctheta_root, &p->ntheta_root);
    }
    /*
    printf("%f\n", p->wcapac_topsoil);
    printf("%f\n\n", p->wcapac_root);

    printf("%f\n", p->ctheta_topsoil);
    printf("%f\n", p->ntheta_topsoil);
    printf("%f\n", p->ctheta_root);
    printf("%f\n", p->ntheta_root);
    printf("%f\n", p->rooting_depth);

    exit(1); */



    free(fsoil_top);
    free(fsoil_root);

    return;
}


double *get_soil_fracs(char *soil_type) {
    /* Based on Table 2 in Cosby et al 1984, page 2.
    Fractions of silt, sand and clay (in that order)
    */
    double *fsoil = malloc(3 * sizeof(double));

    if (strcmp(soil_type, "sand") == 0) {
        fsoil[0] = 0.05;
        fsoil[1] = 0.92;
        fsoil[2] = 0.03;
    } else if (strcmp(soil_type, "loamy_sand") == 0) {
        fsoil[0] = 0.12;
        fsoil[1] = 0.82;
        fsoil[2] = 0.06;
    } else if (strcmp(soil_type, "sandy_loam") == 0) {
        fsoil[0] = 0.32;
        fsoil[1] = 0.58;
        fsoil[2] = 0.1;
    } else if (strcmp(soil_type, "loam") == 0) {
        fsoil[0] = 0.39;
        fsoil[1] = 0.43;
        fsoil[2] = 0.18;
    } else if (strcmp(soil_type, "silty_loam") == 0) {
        fsoil[0] = 0.7;
        fsoil[1] = 0.17;
        fsoil[2] = 0.13;
    } else if (strcmp(soil_type, "sandy_clay_loam") == 0) {
        fsoil[0] = 0.15;
        fsoil[1] = 0.58;
        fsoil[2] = 0.27;
    } else if (strcmp(soil_type, "clay_loam") == 0) {
        fsoil[0] = 0.34;
        fsoil[1] = 0.32;
        fsoil[2] = 0.34;
    } else if (strcmp(soil_type, "silty_clay_loam") == 0) {
        fsoil[0] = 0.56;
        fsoil[1] = 0.1;
        fsoil[2] = 0.34;
    } else if (strcmp(soil_type, "sandy_clay") == 0) {
        fsoil[0] = 0.06;
        fsoil[1] = 0.52;
        fsoil[2] = 0.42;
    } else if (strcmp(soil_type, "silty_clay") == 0) {
        fsoil[0] = 0.47;
        fsoil[1] = 0.06;
        fsoil[2] = 0.47;
    } else if (strcmp(soil_type, "clay") == 0) {
        fsoil[0] = 0.2;
        fsoil[1] = 0.22;
        fsoil[2] = 0.58;
    } else {
        prog_error("Could not understand soil type", __LINE__);
    }

    return (fsoil);
}

void get_soil_params(char *soil_type, double *c_theta, double *n_theta) {
    /* For a given soil type, get the parameters for the soil
    moisture availability based on Landsberg and Waring, with updated
    parameters from Landsberg and Sands (2011), pg 190, Table 7.1

    Table also has values from Saxton for soil texture, perhaps makes more
    sense to use those than Cosby? Investigate?

    Reference
    ---------
    * Landsberg and Sands (2011) Physiological ecology of forest production.
    * Landsberg and Waring (1997) Forest Ecology & Management, 95, 209-228.
    */

    if (strcmp(soil_type, "clay") == 0) {
        *c_theta = 0.4;
        *n_theta = 3.0;
    } else if (strcmp(soil_type, "clay_loam") == 0) {
        *c_theta = 0.5;
        *n_theta = 5.0;
    } else if (strcmp(soil_type, "loam") == 0) {
        *c_theta = 0.55;
        *n_theta = 6.0;
    } else if (strcmp(soil_type, "loamy_sand") == 0) {
        *c_theta = 0.65;
        *n_theta = 8.0;
    } else if (strcmp(soil_type, "sand") == 0) {
        *c_theta = 0.7;
        *n_theta = 9.0;
    } else if (strcmp(soil_type, "sandy_clay") == 0) {
        *c_theta = 0.45;
        *n_theta = 4.0;
    } else if (strcmp(soil_type, "sandy_clay_loam") == 0) {
        *c_theta = 0.525;
        *n_theta = 5.5;
    } else if (strcmp(soil_type, "sandy_loam") == 0) {
        *c_theta = 0.6;
        *n_theta = 7.0;
    } else if (strcmp(soil_type, "silt") == 0) {
        *c_theta = 0.625;
        *n_theta = 7.5;
    } else if (strcmp(soil_type, "silty_clay") == 0) {
        *c_theta = 0.425;
        *n_theta = 3.5;
    } else if (strcmp(soil_type, "silty_clay_loam") == 0) {
        *c_theta = 0.475;
        *n_theta = 4.5;
    } else if (strcmp(soil_type, "silty_loam") == 0) {
        *c_theta = 0.575;
        *n_theta = 6.5;
    } else {
        prog_error("There are no parameters for your soil type", __LINE__);
    }

    return;
}

void calc_soil_params(double *fsoil, double *theta_fc, double *theta_wp,
                      double *theta_sp, double *b, double *psi_sat_mpa) {
    /* Cosby parameters for use within the Clapp Hornberger soil hydraulics
    scheme are calculated based on the texture components of the soil.

    NB: Cosby et al were ambiguous in their paper as to what log base to
    use.  The correct implementation is base 10, as below.

    Parameters:
    ----------
    fsoil : list
        fraction of silt, sand, and clay (in that order

    Returns:
    --------
    theta_fc : float
        volumetric soil water concentration at field capacity
    theta_wp : float
        volumetric soil water concentration at the wilting point

    */
    /* soil suction of 3.364m and 152.9m, or equivalent of -0.033 & -1.5 MPa */
    double pressure_head_wilt = -152.9;
    double pressure_head_crit = -3.364;
    double KPA_2_MPA, METER_OF_HEAD_TO_MPA, psi_sat;

    /* *Note* subtle unit change to be consistent with fractions as opposed
      to percentages of sand, silt, clay, e.g. I've changed the slope in
      the "b" Clapp paramter from 0.157 to 15.7

      Also Cosby is unclear about which log base were used. 'Generally' now
      assumed that logarithms to the base 10
    */

    /* Clapp Hornberger exponent [-] */
    *b = 3.1 + 15.7 * fsoil[CLAY] - 0.3 * fsoil[SAND];

    /* soil matric potential at saturation, taking inverse of log (base10)
      units = m (0.01 converts from mm to m) */
    psi_sat = 0.01 * -(pow(10.0, (1.54 - 0.95 * fsoil[SAND] + 0.63 * fsoil[SILT])));

    /* Height (m) x gravity (m/s2) = pressure (kPa) */
    KPA_2_MPA = 0.001;
    METER_OF_HEAD_TO_MPA = 9.81 * KPA_2_MPA;
    *psi_sat_mpa = psi_sat * METER_OF_HEAD_TO_MPA;

    /* volumetric soil moisture concentrations at the saturation point */
    *theta_sp = 0.505 - 0.037 * fsoil[CLAY] - 0.142 * fsoil[SAND];

    /* volumetric soil moisture concentrations at the wilting point
       assumed to equal suction of -1.5 MPa or a depth of water of 152.9 m */
    *theta_wp = *theta_sp * pow((psi_sat / pressure_head_wilt), (1.0 / *b));

    /* volumetric soil moisture concentrations at field capacity assumed to
       equal a suction of -0.0033 MPa or a depth of water of 3.364 m */
    *theta_fc = *theta_sp * pow((psi_sat / pressure_head_crit), (1.0 / *b));

    return;

}

void calculate_soil_water_fac(control *c, params *p, state *s) {
    /* Estimate a relative water availability factor [0..1]

    A drying soil results in physiological stress that can induce stomatal
    closure and reduce transpiration. Further, N mineralisation depends on
    top soil moisture.

    s->qs = 0.2 in SDGVM

    References:
    -----------
    * Landsberg and Waring (1997) Forest Ecology and Management, 95, 209-228.
      See --> Figure 2.
    * Egea et al. (2011) Agricultural Forest Meteorology, 151, 1370-1384.

    But similarly see:
    * van Genuchten (1981) Soil Sci. Soc. Am. J, 44, 892--898.
    * Wang and Leuning (1998) Ag Forest Met, 91, 89-111.

    * Pepper et al. (2008) Functional Change Biology, 35, 493-508

    Returns:
    --------
    wtfac_topsoil : float
        water availability factor for the top soil [0,1]
    wtfac_root : float
        water availability factor for the root zone [0,1]
    */

    double smc_topsoil, smc_root, psi_swp_topsoil, arg1, arg2, arg3,
           psi_swp_root, b;

    /* turn into fraction... */
    smc_topsoil = s->pawater_topsoil / p->wcapac_topsoil;
    smc_root = s->pawater_root / p->wcapac_root;

    if (c->sw_stress_model == 0) {
        s->wtfac_topsoil = pow(smc_topsoil, p->qs);
        s->wtfac_root = pow(smc_root, p->qs);

    } else if (c->sw_stress_model == 1) {
        s->wtfac_topsoil = calc_sw_modifier(smc_topsoil, p->ctheta_topsoil,
                                            p->ntheta_topsoil);


        s->wtfac_root = calc_sw_modifier(smc_root, p->ctheta_root,
                                         p->ntheta_root);

    } else if (c->sw_stress_model == 2) {

        /* Stomatal limitaiton
           Exponetial function to reduce g1 with soil water limitation
           based on Zhou et al. 2013, AFM, following Makela et al 1996.
           For the moment I have hardwired the PFT parameter as I am still
           testing.
           Because the model is a daily model we are assuming that LWP is
           well approximated by the night SWP. */

        if (float_eq(smc_topsoil, 0.0)) {
            psi_swp_topsoil = -1.5;
        } else {
            arg1 = s->psi_sat_topsoil;
            arg2 = smc_topsoil /s->theta_sat_topsoil;
            arg3 = s->b_topsoil;
            psi_swp_topsoil = arg1 * pow(arg2, arg3);
        }

        if (float_eq(smc_root, 0.0)) {
            psi_swp_root = -1.5;
        } else {
            arg1 = s->psi_sat_root;
            arg2 = smc_root/s->theta_sat_root;
            arg3 = s->b_root;
            psi_swp_root = arg1 * pow(arg2, arg3);
        }

        /* multipliy these by g1, same as eqn 3 in Zhou et al. 2013. */
        b = 0.66;

        s->wtfac_topsoil = exp(b * psi_swp_topsoil);
        s->wtfac_root = exp(b * psi_swp_root);
    }

    return;
}

double calc_sw_modifier(double theta, double c_theta, double n_theta) {
    /* From Landsberg and Waring */
    return (1.0  / (1.0 + pow(((1.0 - theta) / c_theta), n_theta)));
}

void sum_hourly_water_fluxes(fluxes *f, double soil_evap_hlf_hr,
                             double transpiration_hlf_hr, double et_hlf_hr,
                             double interception_hlf_hr,
                             double runoff_hlf_hr) {

    /* add half hour fluxes to day total store */
    f->soil_evap += soil_evap_hlf_hr;
    f->transpiration += transpiration_hlf_hr;
    f->et += et_hlf_hr;
    f->interception += interception_hlf_hr;
    f->runoff += runoff_hlf_hr;

    return;
}

void update_daily_water_struct(fluxes *f, double day_soil_evap,
                               double day_transpiration, double day_et,
                               double day_interception, double day_runoff) {

    /* add half hour fluxes to day total store */
    f->soil_evap = day_soil_evap;
    f->transpiration = day_transpiration;
    f->et = day_et;
    f->interception = day_interception;
    f->runoff = day_runoff;

    return;
}

void zero_water_day_fluxes(fluxes *f) {

    f->et = 0.0;
    f->soil_evap = 0.0;
    f->transpiration = 0.0;
    f->interception = 0.0;
    f->runoff = 0.0;
    f->gs_mol_m2_sec = 0.0;

    return;
}

double calc_radiation_conductance(double tair) {
    /*  Returns the 'radiation conductance' at given temperature.

        Units: mol m-2 s-1

        References:
        -----------
        * Formula from Ying-Ping's version of Maestro, cf. Wang and Leuning
          1998, Table 1,
        * See also Jones (1992) p. 108.
        * And documented in Medlyn 2007, equation A3, although I think there
          is a mistake. It should be Tk**3 not Tk**4, see W & L.
    */
    double grad;
    double Tk;

    Tk = tair + DEG_TO_KELVIN;
    grad = 4.0 * SIGMA * (Tk * Tk * Tk) * LEAF_EMISSIVITY / (CP * MASS_AIR);

    return (grad);
}

double calc_bdn_layer_forced_conduct(double tair, double press, double wind,
                                     double leaf_width) {
    /*
        Boundary layer conductance for heat - single sided, forced convection
        (mol m-2 s-1)
        See Leuning et al (1995) PC&E 18:1183-1200 Eqn E1
    */
    double cmolar, Tk, gbh;

    Tk = tair + DEG_TO_KELVIN;
    cmolar = press / (RGAS * Tk);
    gbh = 0.003 * sqrt(wind / leaf_width) * cmolar;

    return (gbh);
}

double calc_bdn_layer_free_conduct(double tair, double tleaf, double press,
                                   double leaf_width) {
    /*
        Boundary layer conductance for heat - single sided, free convection
        (mol m-2 s-1)
        See Leuning et al (1995) PC&E 18:1183-1200 Eqns E3 & E4
    */
    double cmolar, Tk, gbh, grashof, leaf_width_cubed;
    double tolerance = 1E-08;

    Tk = tair + DEG_TO_KELVIN;
    cmolar = press / (RGAS * Tk);
    leaf_width_cubed = leaf_width * leaf_width * leaf_width;

    if (float_eq((tleaf - tair), 0.0)) {
        gbh = 0.0;
    } else {
        grashof = 1.6E8 * fabs(tleaf - tair) * leaf_width_cubed;
        gbh = 0.5 * DHEAT * pow(grashof, 0.25) / leaf_width * cmolar;
    }

    return (gbh);
}
