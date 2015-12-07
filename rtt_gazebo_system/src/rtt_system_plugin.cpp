
#include <cstdlib>

// Boost
#include <boost/bind.hpp>
#include <boost/thread/locks.hpp>

// Gazebo
#include <gazebo/gazebo.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo/common/common.hh>

// Orocos
#include <rtt/deployment/ComponentLoader.hpp>
#include <ocl/DeploymentComponent.hpp>
#include <ocl/TaskBrowser.hpp>
#include <ocl/LoggingService.hpp>
#include <rtt/Logger.hpp>


#include <rtt/os/startstop.h>
#include <rtt/scripting/Scripting.hpp>
#include <rtt/transports/corba/corba.h>
#include <rtt/transports/corba/TaskContextServer.hpp>

// RTT/ROS Simulation Clock Activity
#include <rtt_rosclock/rtt_rosclock.h>

#ifdef RTT_GAZEBO_DEBUG
#include <rtt_rosclock/prof.h>
#include <rtt_rosclock/throttle.h>
#endif

#include "rtt_system_plugin.h"

using namespace rtt_gazebo_system;

void RTTSystemPlugin::Load(int argc, char **argv)
{
  // Initialize RTT
  __os_init(argc, argv);

  RTT::Logger::log().setStdStream(std::cerr);
  RTT::Logger::log().mayLogStdOut(true);
  //RTT::Logger::log().setLogLevel(RTT::Logger::Info);

  // Setup TaskContext server if necessary
  if(CORBA::is_nil(RTT::corba::TaskContextServer::orb)) {
    // Initialize orb
    RTT::corba::TaskContextServer::InitOrb(argc, argv);
    // Propcess orb requests in a thread
    RTT::corba::TaskContextServer::ThreadOrb();
  }
}

void RTTSystemPlugin::Init()
{
  // Initialize and enable the simulation clock
  rtt_rosclock::use_manual_clock();
  rtt_rosclock::enable_sim();

  update_connection_ =
    gazebo::event::Events::ConnectWorldUpdateEnd(
        boost::bind(&RTTSystemPlugin::updateClock, this));

  stop_connection_ =
    gazebo::event::Events::ConnectStop(
        boost::bind(&RTTSystemPlugin::stop, this));

  // Start update thread
  simulate_clock_ = true;
  update_thread_ = boost::thread(
      boost::bind(&RTTSystemPlugin::updateClockLoop, this));
}

RTTSystemPlugin::~RTTSystemPlugin()
{
  // Stop the Orb thread
  if(!CORBA::is_nil(RTT::corba::TaskContextServer::orb)) {
    RTT::corba::TaskContextServer::ShutdownOrb();
    RTT::corba::TaskContextServer::DestroyOrb();
    RTT::corba::TaskContextServer::CleanupServers();
  }
}

void RTTSystemPlugin::updateClock()
{
  // Notify the update clock loop
  boost::lock_guard<boost::mutex> lock(update_mutex_);
  update_cond_.notify_one();
}

void RTTSystemPlugin::stop()
{
  simulate_clock_ = false;
}

void RTTSystemPlugin::updateClockLoop()
{
  // Wait for update signal to start the loop
  {
    boost::unique_lock<boost::mutex> lock(update_mutex_);
    update_cond_.wait(lock);
  }

  while(simulate_clock_)
  {
#ifdef RTT_GAZEBO_DEBUG
    static rtt_rosclock::WallProf prof(5.0);
    static rtt_rosclock::WallThrottle throttle(ros::Duration(1.0));
    prof.tic();
#endif

    // Get the simulation time
    if(!gazebo::physics::worlds_running()) {
      break;
    }
    const gazebo::physics::WorldPtr world = gazebo::physics::get_world();
    if(!world) {
      break;
    }
    gazebo::common::Time gz_time = world->GetSimTime();

    // Update the clock from the simulation time and execute the SimClockActivities
    // NOTE: all orocos TaskContexts which use a SimClockActivity are updated within this blocking call
    rtt_rosclock::update_sim_clock(ros::Time(gz_time.sec, gz_time.nsec));

#ifdef RTT_GAZEBO_DEBUG
    prof.toc();
    if(throttle.ready()) {
      prof.analyze();
      RTT::log(RTT::Debug) << prof.mean() << " +/- " << prof.stddev() <<" [s] ("<<prof.n()<<") for update_sim_clock()" << RTT::endlog();
    }
    static ros::Time last_update_time = rtt_rosclock::rtt_wall_now();
#endif
  }
}

GZ_REGISTER_SYSTEM_PLUGIN(rtt_gazebo_system::RTTSystemPlugin)
