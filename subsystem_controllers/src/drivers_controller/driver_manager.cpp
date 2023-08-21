/*
 * Copyright (C) 2023 LEIDOS.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */


#include <boost/algorithm/string.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <rclcpp/logger.hpp>
#include <rclcpp/logging.hpp>
#include "subsystem_controllers/drivers_controller/driver_manager.hpp"


using std_msec = std::chrono::milliseconds;

namespace subsystem_controllers
{
    DriverManager::DriverManager(const std::vector<std::string>& critical_driver_names,
                        const std::vector<std::string>& lidar_gps_entries,
                        const std::vector<std::string>& camera_entries,
                        const std::vector<std::string>& base_managed_ros2_nodes,
                        std::shared_ptr<ros2_lifecycle_manager::LifecycleManagerInterface> driver_lifecycle_mgr,
                        GetParentNodeStateFunc get_parent_state_func,
                        ServiceNamesAndTypesFunc get_service_names_and_types_func,
                        const long driver_timeout)
            : critical_drivers_(critical_driver_names.begin(), critical_driver_names.end()),
            lidar_gps_entries_(lidar_gps_entries.begin(), lidar_gps_entries.end()),
            camera_entries_(camera_entries.begin(), camera_entries.end()),
            ros2_drivers_(base_managed_ros2_nodes.begin(), base_managed_ros2_nodes.end()),
            get_parent_state_func_(get_parent_state_func),
            driver_lifecycle_mgr_(driver_lifecycle_mgr),
            get_service_names_and_types_func_(get_service_names_and_types_func),
            driver_timeout_(driver_timeout)
    { 
        // Intialize entry manager
        em_ = std::make_shared<EntryManager>(critical_driver_names, lidar_gps_entries, camera_entries);
        // Create entries for ros2 drivers
        for (const auto& p: ros2_drivers_)
        {
            Entry ros2_driver_status(false, false, p, 0, "", false);
            em_->update_entry(ros2_driver_status);
        }

    }
    

    carma_msgs::msg::SystemAlert DriverManager::handle_spin(bool truck,bool car,long time_now,long start_up_timestamp,long startup_duration)
    {
        carma_msgs::msg::SystemAlert alert;

        if(truck==true)
        {
            std::string status = are_critical_drivers_operational_truck(time_now);
            if(status.compare("s_1_l1_1_l2_1_g_1_c_1") == 0)
            {
                starting_up_ = false;
                alert.description = "All essential drivers are ready";
                alert.type = carma_msgs::msg::SystemAlert::DRIVERS_READY;
                return alert;
            } 
           else if(starting_up_ && (time_now - start_up_timestamp <= startup_duration))
            {
                alert.description = "System is starting up...";
                alert.type = carma_msgs::msg::SystemAlert::NOT_READY;
                return alert;
            }
             else if(status.compare("s_1_l1_1_l2_1_g_1_c_0")==0)
            {
                alert.description = "Camera Failed";
                alert.type = carma_msgs::msg::SystemAlert::SHUTDOWN;
            }
            else if((status.compare("s_1_l1_0_l2_1_g_1") == 0) || (status.compare("s_1_l1_1_l2_0_g_1") == 0))
            {
            
                alert.description = "One LIDAR Failed";
                alert.type = carma_msgs::msg::SystemAlert::CAUTION;
                return alert;
            }
            else if((status.compare("s_1_l1_0_l2_1_g_0") == 0) || (status.compare("s_1_l1_1_l2_0_g_0") == 0))
            {   
                alert.description = "One Lidar and GPS Failed";
                alert.type = carma_msgs::msg::SystemAlert::CAUTION;
                return alert;
            } 
            else if(status.compare("s_1_l1_1_l2_1_g_0") == 0)
            {
                alert.description = "GPS Failed";
                alert.type = carma_msgs::msg::SystemAlert::CAUTION;
                return alert;
            }
            else if(status.compare("s_1_l1_0_l2_0_g_1") == 0)
            {
                alert.description = "Both LIDARS Failed";
                alert.type = carma_msgs::msg::SystemAlert::WARNING;
                return alert;
            }
            else if(status.compare("s_1_l1_0_l2_0_g_0") == 0)
            {
                alert.description = "LIDARS and GPS Failed";
                alert.type = carma_msgs::msg::SystemAlert::SHUTDOWN;
                return alert;
            }
            else if(status.compare("s_0") == 0)
            {
                alert.description = "SSC Failed";
                alert.type = carma_msgs::msg::SystemAlert::SHUTDOWN;
                return alert;
            }
            else
            {
                alert.description = "Unknown problem assessing essential driver availability";
                alert.type = carma_msgs::msg::SystemAlert::FATAL;
                return alert;  
            }
 
        }
        else if(car==true)
        {
            std::string status = are_critical_drivers_operational_car(time_now);
            if(status.compare("s_1_l_1_g_1_c_1") == 0)
            {
                starting_up_ = false;
                alert.description = "All essential drivers are ready";
                alert.type = carma_msgs::msg::SystemAlert::DRIVERS_READY;
                return alert; 
            }
            else if(starting_up_ && (time_now - start_up_timestamp <= startup_duration))
            {
                alert.description = "System is starting up...";
                alert.type = carma_msgs::msg::SystemAlert::NOT_READY;
                return alert; 
            }
            
            else if(status.compare("s_1_l_1_g_1_c_0") == 0)
            {
                alert.description = "Camera Failed";
                alert.type = carma_msgs::msg::SystemAlert::SHUTDOWN;
                return alert;
            } 
            else if(status.compare("s_1_l_1_g_0") == 0)
            {
                alert.description = "GPS Failed";
                alert.type = carma_msgs::msg::SystemAlert::CAUTION;
                return alert; 
            }
            else if(status.compare("s_1_l_0_g_1") == 0)
            {
                alert.description = "LIDAR Failed";
                alert.type = carma_msgs::msg::SystemAlert::WARNING;
                return alert; 
            }
            else if(status.compare("s_1_l_0_g_0") == 0)
            {
                alert.description = "LIDAR, GPS Failed";
                alert.type = carma_msgs::msg::SystemAlert::SHUTDOWN;
                return alert; 
            }
            else if(status.compare("s_1_l_0_g_1_c_0") == 0)
            {
                alert.description = "LIDAR, Camera Failed";
                alert.type = carma_msgs::msg::SystemAlert::SHUTDOWN;
                return alert;
            }
            else if(status.compare("s_1_l_1_g_0_c_0") == 0)
            {
                alert.description = " GPS, Camera Failed";
                alert.type = carma_msgs::msg::SystemAlert::SHUTDOWN;
                return alert;
            }
            else if(status.compare("s_0") == 0)
            {
                alert.description = "SSC Failed";
                alert.type = carma_msgs::msg::SystemAlert::SHUTDOWN;
                return alert; 
            }
            else
            {
                alert.description = "Unknown problem assessing essential driver availability";
                alert.type = carma_msgs::msg::SystemAlert::FATAL;
                return alert;  
            }
        }
        else
        {
            alert.description = "Need to set either truck or car flag";
            alert.type = carma_msgs::msg::SystemAlert::FATAL;
            return alert; 
        }
    
    }


    void DriverManager::update_driver_status(const carma_driver_msgs::msg::DriverStatus::SharedPtr msg, long current_time)
    {
        // update driver status is only called in response to a message received on driver_discovery. This topic is only being published in ros1
        Entry driver_status(msg->status == carma_driver_msgs::msg::DriverStatus::OPERATIONAL || msg->status == carma_driver_msgs::msg::DriverStatus::DEGRADED,
                            true, msg->name, 0, "", true);

        em_->update_entry(driver_status);                            
    }


    void DriverManager::evaluate_sensor(int &sensor_input,bool available,long current_time,long timestamp,long driver_timeout, std::string source_node, bool is_ros1)
    {
        if(is_ros1){
            if((!available) || (current_time-timestamp > driver_timeout))
            {
                sensor_input=0;
            }
            else
            {
                sensor_input=1;
            }
        }
        else{
            // If source node is ros2 check lifecycle manager for active state
            if (driver_lifecycle_mgr_->get_managed_node_state(source_node) == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
            {
                sensor_input = 1;
            }
            else
            {
                sensor_input = 0;
            }
        }
    }

    std::string DriverManager::are_critical_drivers_operational_truck(long current_time)
    {
        int ssc=0;
        int lidar1=0;
        int lidar2=0;
        int gps=0;
        int camera=0; //Add Camera Driver

        std::vector<Entry> driver_list = em_->get_entries(); //Real time driver list from driver status
        for(auto i = driver_list.begin(); i < driver_list.end(); ++i)
        {
            if(em_->is_entry_required(i->name_))
            {
              evaluate_sensor(ssc,i->available_,current_time,i->timestamp_,driver_timeout_, i->name_, i->is_ros1_);
            }

            if(em_->is_lidar_gps_entry_required(i->name_)==0) //Lidar1
            {
               evaluate_sensor(lidar1,i->available_,current_time,i->timestamp_,driver_timeout_, i->name_, i->is_ros1_);
            }
            else if(em_->is_lidar_gps_entry_required(i->name_)==1) //Lidar2
            {
              evaluate_sensor(lidar2,i->available_,current_time,i->timestamp_,driver_timeout_, i->name_, i->is_ros1_);
            }
            else if(em_->is_lidar_gps_entry_required(i->name_)==2) //GPS
            {
              evaluate_sensor(gps,i->available_,current_time,i->timestamp_,driver_timeout_, i->name_, i->is_ros1_);
            }
            else if(em_->is_camera_entry_required(i->name_)==0)
            {
                evaluate_sensor(camera,i->available_,current_time,i->timestamp_,driver_timeout_, i->name_, i->is_ros1_);
            }
            
        }

        //////////////////////
        // NOTE: THIS IS A MANUAL DISABLE OF ALL LIDAR AND GPS FAILURE DETECTION FOLLOWING THE ROS2 PORT
        /////////////////////
        lidar1=1;
        lidar2=1;
        gps=1;
        /////////////////////

        //Decision making 
        if (ssc == 0)
        {
            return "s_0";
        }
        // if ssc= 1
        if((lidar1==0) && (lidar2==0) && (gps==0))
        {
            return "s_1_l1_0_l2_0_g_0";
        }
        else if((lidar1==0) && (lidar2==0) && (gps==1))
        {
            return "s_1_l1_0_l2_0_g_1";
        }
        else if((lidar1==0) && (lidar2==1) && (gps==0))
        {
            return "s_1_l1_0_l2_1_g_0";
        }
        else if((lidar1==0) && (lidar2==1) && (gps==1))
        {
            return "s_1_l1_0_l2_1_g_1";
        }
        else if((lidar1==1) && (lidar2==0) && (gps==0))
        {
            return "s_1_l1_1_l2_0_g_0";
        }
        else if((lidar1==1) && (lidar2==0) && (gps==1))
        {
            return "s_1_l1_1_l2_0_g_1";
        }
        else if((lidar1==1) && (lidar2==1) && (gps==0))
        {
            return "s_1_l1_1_l2_1_g_0";
        }
        else if ((camera==0) && (lidar1 ==1) && (lidar2 == 1) && (gps == 1) )
        {
            return "s_1_l1_1_l2_1_g_1_c_0";
        }
        else if((lidar1==1) && (lidar2==1) && (gps==1) && (camera == 1))
        {
            return "s_1_l1_1_l2_1_g_1_c_1";
        }
        
    }


    std::string DriverManager::are_critical_drivers_operational_car(long current_time)
    {
        int ssc=0;
        int lidar=0;
        int gps=0;
        int camera=0;
        std::vector<Entry> driver_list = em_->get_entries(); //Real time driver list from driver status
        for(auto i = driver_list.begin(); i < driver_list.end(); ++i)
        {
            if(em_->is_entry_required(i->name_))
            {
                evaluate_sensor(ssc,i->available_,current_time,i->timestamp_,driver_timeout_, i->name_, i->is_ros1_);
            }
            if(em_->is_lidar_gps_entry_required(i->name_)==0) //Lidar
            {   
                evaluate_sensor(lidar,i->available_,current_time,i->timestamp_,driver_timeout_, i->name_, i->is_ros1_);
            }
            else if(em_->is_lidar_gps_entry_required(i->name_)==1) //GPS
            {
                evaluate_sensor(gps,i->available_,current_time,i->timestamp_,driver_timeout_, i->name_, i->is_ros1_);
            }
            else if(em_->is_camera_entry_required(i->name_)==0)
            {
                evaluate_sensor(camera,i->available_,current_time,i->timestamp_,driver_timeout_, i->name_, i->is_ros1_);
            }
        }

        //////////////////////
        // NOTE: THIS IS A MANUAL DISABLE OF ALL LIDAR FAILURE DETECTION FOLLOWING THE ROS2 PORT
        /////////////////////
        lidar=1;
        gps=1;
        /////////////////////

        //Decision making 
        if(ssc==1)
        {
            if((lidar==0) && (gps==0))
            {
                return "s_1_l_0_g_0";
            }
            
            else if((lidar==0) && (gps==1) && (camera == 1))
            {
                return "s_1_l_0_g_1";
            }
            else if((lidar==1) && (gps==0) && (camera == 1))
            {
                return "s_1_l_1_g_0";
            }
            else if ((camera==0) && (lidar == 1) && (gps ==1))
            {
                return "s_1_l_1_g_1_c_0";
            }
            else if ((camera==0) && (lidar == 0) && (gps == 1))
            {
                return "s_1_l_0_g_1_c_0";
            }
            else if ((camera==0) && (lidar == 1) && (gps == 0))
            {
                return "s_1_l_1_g_0_c_0";
            }
            else if((lidar==1) && (gps==1) && (camera == 1))
            {
                return "s_1_l_1_g_1_c_1";
            }

        }
        else
        {
            return "s_0";
        }

    }


}