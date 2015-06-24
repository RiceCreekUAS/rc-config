/**
 * \file: task_circle_coord.cxx
 *
 * Task: configure autopilot settings to fly to a circle around a specified
 * point.  Compensate circle track using wind estimate to try to achieve a 
 * better circle form.
 *
 * Copyright (C) 2011 - Curtis L. Olson curtolson@gmail.com
 *
 */

#include <cstdio>
#include <cmath>

#include "comms/display.h"
#include "control/waypoint.hxx"
#include "include/globaldefs.h"
#include "util/wind.hxx"

#include "circle_mgr.hxx"

AuraCircleMgr::AuraCircleMgr( SGPropertyNode *branch ) :
    config_path( "" ),
    _direction( "left" ),
    _radius_m( 100.0 ),
    _target_agl_ft( 0.0 ),
    _target_speed_kt( 0.0 ),

    lon_node( NULL ),
    lat_node( NULL ),
    alt_agl_node( NULL ),
    true_heading_node( NULL ),
    groundtrack_node( NULL ),
    groundspeed_node( NULL ),
    coord_lon_node( NULL ),
    coord_lat_node( NULL ),

    direction_node( NULL ),
    radius_node( NULL ),
    bank_limit_node( NULL ),
    L1_period_node( NULL ),
    override_agl_node( NULL ),
    override_speed_node( NULL ),

    exit_agl_node( NULL ),
    exit_heading_node( NULL ),
    fcs_mode_node( NULL ),
    ap_speed_node( NULL ),
    ap_agl_node( NULL ),
    ap_roll_node( NULL ),
    target_course_deg( NULL ),
    wp_dist_m( NULL ),
    wp_eta_sec( NULL ),

    saved_fcs_mode( "" ),
    saved_agl_ft( 0.0 ),
    saved_speed_kt( 0.0 ),
    saved_direction( "" ),
    saved_radius_m( 0.0 )
{
    int i;
    SGPropertyNode *node;
    int count = branch->nChildren();
    for ( i = 0; i < count; ++i ) {
        node = branch->getChild(i);
        string name = node->getName();
	if ( name == "config" ) {
	    config_path = node->getStringValue();
	} else if ( name == "direction" ) {
	    _direction = node->getStringValue();
	} else if ( name == "radius-m" ) {
	    _radius_m = node->getDoubleValue();
	} else if ( name == "altitude-agl-ft" ) {
	    _target_agl_ft = node->getDoubleValue();
	} else if ( name == "speed-kt" ) {
	    _target_speed_kt = node->getDoubleValue();
        } else {
            printf("Unknown circle task parameter: %s\n", name.c_str() );
        }
    }
};


AuraCircleMgr::~AuraCircleMgr() {
};


bool AuraCircleMgr::bind() {
    lon_node = fgGetNode( "/position/longitude-deg", true );
    lat_node = fgGetNode( "/position/latitude-deg", true );
    alt_agl_node = fgGetNode("/position/altitude-agl-ft", true);
    true_heading_node = fgGetNode( "/orientation/heading-deg", true );
    groundtrack_node = fgGetNode( "/orientation/groundtrack-deg", true );
    groundspeed_node = fgGetNode("/velocity/groundspeed-ms", true);

    string prop;

    prop = config_path + "/"; prop += "longitude-deg";
    coord_lon_node = fgGetNode(prop.c_str(), true );

    prop = config_path + "/"; prop += "latitude-deg";
    coord_lat_node = fgGetNode(prop.c_str(), true );

    prop = config_path + "/"; prop += "direction";
    direction_node = fgGetNode(prop.c_str(), true );

    prop = config_path + "/"; prop += "radius-m";
    radius_node = fgGetNode(prop.c_str(), true );

    prop = config_path + "/"; prop += "altitude-agl-ft";
    override_agl_node = fgGetNode(prop.c_str(), true );
    if ( _target_agl_ft > 0.0 ) {
	override_agl_node->setDoubleValue(_target_agl_ft);
    }

    prop = config_path + "/"; prop += "speed-kt";
    override_speed_node = fgGetNode(prop.c_str(), true );
    if ( _target_speed_kt > 0.0 ) {
	override_speed_node->setDoubleValue(_target_speed_kt);
    }

    prop = config_path + "/"; prop += "exit-agl-ft";
    exit_agl_node = fgGetNode(prop.c_str(), true );

    prop = config_path + "/"; prop += "exit-heading-deg";
    exit_heading_node = fgGetNode(prop.c_str(), true );

    fcs_mode_node = fgGetNode("/config/fcs/mode", true);

    bank_limit_node = fgGetNode("/config/fcs/autopilot/L1-controller/bank-limit-deg", true );
    L1_period_node = fgGetNode("/config/fcs/autopilot/L1-controller/period", true );
    // sanity check, set some conservative values if none are provided
    // in the autopilot config
    if ( bank_limit_node->getDoubleValue() < 0.1 ) {
	bank_limit_node->setDoubleValue( 20.0 );
    }
    if ( L1_period_node->getDoubleValue() < 0.1 ) {
	L1_period_node->setDoubleValue( 25.0 );
    }

    ap_speed_node = fgGetNode("/autopilot/settings/target-speed-kt", true );
    ap_agl_node = fgGetNode("/autopilot/settings/target-agl-ft", true );
    ap_roll_node = fgGetNode("/autopilot/settings/target-roll-deg", true);
    target_course_deg = fgGetNode( "/autopilot/settings/target-groundtrack-deg", true );

    wp_dist_m = fgGetNode( "/mission/route/wp-dist-m", true );
    wp_eta_sec = fgGetNode( "/mission/route/wp-eta-sec", true );

    return true;
}


bool AuraCircleMgr::init() {
    bind();

    return true;
}


bool AuraCircleMgr::update() {
    // printf("circle update\n");

    string direction_str = direction_node->getStringValue();
    double direction = 1.0;
    if ( direction_str == (string)"right" ) {
	direction = -1.0;
    }

    SGWayPoint target = SGWayPoint( coord_lon_node->getDoubleValue(),
				    coord_lat_node->getDoubleValue() );

    double course_deg;
    double dist_m;
    target.CourseAndDistance( lon_node->getDoubleValue(),
			      lat_node->getDoubleValue(),
			      0.0, &course_deg, &dist_m );

    // compute ideal ground course if at ideal radius
    double ideal_crs = course_deg + direction * 90;
    if ( ideal_crs > 360.0 ) { ideal_crs -= 360.0; }
    if ( ideal_crs < 0.0 ) { ideal_crs += 360.0; }

    // compute the ideal radius for a "bank_deg" degree bank in no
    // wind; use target speed to compute ideal radius rather than
    // current airspeed so we don't have to chase a moving target.

    // this will compute a radius based on aircraft current airspeed
    // double speed_kt = true_airspeed_kt->getDoubleValue();

    // this will compute radius based on the current ap set speed
    // double speed_kt = ap_speed_node->getDoubleValue();

    //double radius_ft = (speed_kt * speed_kt)
    //   / (11.23 * tan(bank_node->getDoubleValue() * SG_DEGREES_TO_RADIANS));
    // double radius_m = radius_ft * SG_FEET_TO_METER;

    // (in)sanity check
    double radius_m = radius_node->getDoubleValue();
    if ( radius_m < 10.0 ) { radius_m = 10.0; }

    // compute a target ground course based on our actual radius distance
    double target_crs = ideal_crs;
    if ( dist_m < radius_m ) {
	// inside circle, adjust target heading to expand our circling
	// radius
	double offset_deg = direction * 90.0 * (1.0 - dist_m / radius_m);
	target_crs += offset_deg;
	if ( target_crs > 360.0 ) { target_crs -= 360.0; }
	if ( target_crs < 0.0 ) { target_crs += 360.0; }
    } else if ( dist_m > radius_m ) {
	// outside circle, adjust target heading to tighten our
	// circling radius
	double offset_dist = dist_m - radius_m;
	if ( offset_dist > radius_m ) { offset_dist = radius_m; }
	double offset_deg = direction * 90 * offset_dist / radius_m;
	target_crs -= offset_deg;
	if ( target_crs > 360.0 ) { target_crs -= 360.0; }
	if ( target_crs < 0.0 ) { target_crs += 360.0; }
    }
    target_course_deg->setDoubleValue( target_crs );
    /*if ( display_on ) {
	printf("rad=%.0f act=%.0f ideal crs=%.1f tgt crs=%.1f\n",
	       radius_m, dist_m, ideal_crs, target_crs);
      }*/

    // new L1 'mathematical' response to error

    double L1_period = L1_period_node->getDoubleValue();	// gain
    double gs_mps = groundspeed_node->getDoubleValue();
    const double sqrt_of_2 = 1.41421356237309504880;
    double omegaA = sqrt_of_2 * SGD_PI / L1_period;
    double VomegaA = gs_mps * omegaA;
    double course_error = groundtrack_node->getDoubleValue() - target_crs;
    if ( course_error < -180.0 ) { course_error += 360.0; }
    if ( course_error >  180.0 ) { course_error -= 360.0; }
    double accel = 2.0 * sin(course_error * SG_DEGREES_TO_RADIANS) * VomegaA;
    // double accel = 2.0 * gs_mps * gs_mps * sin(course_error * SG_DEGREES_TO_RADIANS) / L1;
    double ideal_accel = direction * gs_mps * gs_mps / radius_m;
    double total_accel = ideal_accel + accel;
    static const double gravity = 9.81; // m/sec^2
    double target_bank = -atan( total_accel / gravity );
    double target_bank_deg = target_bank * SG_RADIANS_TO_DEGREES;

    double bank_limit_deg = bank_limit_node->getDoubleValue();
    if ( target_bank_deg < -bank_limit_deg ) {
	target_bank_deg = -bank_limit_deg;
    }
    if ( target_bank_deg > bank_limit_deg ) {
	target_bank_deg = bank_limit_deg;
    }
    //printf("   circle: tgt bank = %.0f  bank limit = %.0f\n",
    //	   target_bank_deg, bank_limit_deg);

    ap_roll_node->setDoubleValue( target_bank_deg );

    // printf("circle: ground_crs = %.1f aircraft_hdg = %.1f\n",
    //	   course_deg, hd_deg );

    wp_dist_m->setFloatValue( dist_m );
    if ( gs_mps > 0.1 ) {
	wp_eta_sec->setFloatValue( dist_m / gs_mps );
    } else {
	wp_eta_sec->setFloatValue( 0.0 );
    }

#if 0

    // attempt to intelligently/smoothly descend and arrive at the
    // correct altitude simultaneously with the correct exit point.
    // Alas, the current logic is lacking a certain element of, how do
    // you say it: correctness.

    // if an exit condition is set, compute the distance remaining
    double exit_agl_ft = exit_agl_node->getDoubleValue();
    if ( exit_agl_ft > 0.0 ) {
	double exit_heading_deg = exit_heading_node->getDoubleValue();
	double heading_deg = groundtrack_node->getDoubleValue();
	if ( heading_deg < 0.0 ) { heading_deg += 360.0; }
	double diff = 0.0;
	if ( direction < 0.0 ) {
	    diff = exit_heading_deg - heading_deg;
	} else {
	    diff = 360 - (exit_heading_deg - heading_deg);
	}
	if ( diff < 0.0 ) { diff += 360.0; }
	if ( diff > 360.0 ) { diff -= 360.0; }

	double dist_m = 2.0 * radius_m * (diff / 360.0) * SG_PI;
	/*if ( display_on ) {
	    printf( "distance to exit point = %.1f\n", dist_m );
	    }*/

	SGPropertyNode *glideslope_node
	    = fgGetNode("/mission/land/glideslope-deg", true);
	double glideslope_rad = glideslope_node->getDoubleValue()
	    * SG_DEGREES_TO_RADIANS;
	if ( glideslope_rad < 0.001 ) { glideslope_rad = 0.524; /* 3 deg */ }
	if ( glideslope_rad > 0.8 ) { glideslope_rad = 0.524; }

	double exit_agl_ft = exit_agl_node->getDoubleValue();
	double alt_agl_ft = alt_agl_node->getDoubleValue();
	double alt_error = alt_agl_ft - exit_agl_ft;
	double dist_needed = alt_error*SG_FEET_TO_METER / tan(glideslope_rad);
	double circum_m = 2.0 * radius_m * SG_PI;
	double turns_needed = dist_needed / circum_m;

	double percent_to_exit = diff / 360.0;
	double spread_turns = (int)(turns_needed - 0.1) + percent_to_exit;
	double spread_dist_m = spread_turns * circum_m;
	double spread_alt_ft = spread_dist_m * SG_METER_TO_FEET
	    * tan( glideslope_rad );
	double current_ap_agl_ft = ap_agl_node->getDoubleValue();
	double target_ap_agl_ft = exit_agl_ft + spread_alt_ft;
	if ( display_on ) {
	    printf("alterr=%.0f turns needed=%.1f spread dist=%.0f alt=%.0f ap=%.0f\n",
		   alt_error, turns_needed, spread_dist_m, spread_alt_ft, target_ap_agl_ft);
	}
	ap_agl_node->setDoubleValue( target_ap_agl_ft );
    }
#endif

    return true;
};


SGWayPoint AuraCircleMgr::get_center() {
    return SGWayPoint( coord_lon_node->getDoubleValue(),
		       coord_lat_node->getDoubleValue() );
}


void AuraCircleMgr::set_direction( const string direction ) {
    direction_node->setStringValue( direction.c_str() );
}

void AuraCircleMgr::set_radius( const double radius_m ) {
    radius_node->setDoubleValue( radius_m );
}
