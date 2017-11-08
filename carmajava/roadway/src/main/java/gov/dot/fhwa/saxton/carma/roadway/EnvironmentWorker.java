/*
 * Copyright (C) 2017 LEIDOS.
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

package gov.dot.fhwa.saxton.carma.roadway;

import cav_msgs.SystemAlert;
import geometry_msgs.TransformStamped;
import gov.dot.fhwa.saxton.carma.geometry.GeodesicCartesianConverter;
import gov.dot.fhwa.saxton.carma.geometry.cartesian.Point3D;
import gov.dot.fhwa.saxton.carma.geometry.geodesic.Location;
import gov.dot.fhwa.saxton.carma.rosutils.SaxtonLogger;
import org.apache.commons.logging.Log;
import org.ros.message.Duration;
import org.ros.message.MessageFactory;
import org.ros.message.Time;
import org.ros.node.NodeConfiguration;
import org.ros.rosjava_geometry.Quaternion;
import org.ros.rosjava_geometry.Transform;
import org.ros.rosjava_geometry.Vector3;
import std_msgs.Header;
import tf2_msgs.TFMessage;

import java.util.Arrays;
import java.util.LinkedList;
import java.util.List;

/**
 * The EnvironmentWorker is responsible for implementing all non pub-sub logic of the EnvironmentManager node
 * Primary responsibility is updating of coordinate transforms and providing a representation of local geometry
 */
public class EnvironmentWorker {
  // Messaging and logging
  protected SaxtonLogger log;
  protected IEnvironmentManager envMgr;
  protected final MessageFactory messageFactory = NodeConfiguration.newPrivate().getTopicMessageFactory();

  // Host vehicle state variables
  protected boolean headingReceived = false;
  protected boolean nabSatFixReceived = false;
  protected Location hostVehicleLocation = null;
  protected double hostVehicleHeading; // The heading of the vehicle in degrees east of north in an NED frame.
  // Frame ids
  protected final String earthFrame = "earth";
  protected final String mapFrame = "map";
  protected final String odomFrame = "odom";
  protected final String baseLinkFrame = "base_link";
  protected final String positionSensorFrame = "pinpoint"; //TODO set this name via ros param
  // Transforms
  protected Transform mapToOdom = Transform.identity();
  protected Transform earthToMap = null;
  protected Transform baseToPositionSensor = null;
  protected Transform odomToBaseLink = Transform.identity(); // The odom frame will start in the same orientation as the base_link frame on startup
  // Transform Update parameters
  protected Time prevMapTime = null;
  protected Duration MAP_UPDATE_PERIOD = new Duration(5); // Time in seconds between updating the map frame location
  protected int tfSequenceCount = 0;

  /**
   * Constructor
   * @param envMgr EnvironmentWorker used to publish data and get stored transforms
   * @param log Logging object
   */
  public EnvironmentWorker(IEnvironmentManager envMgr, Log log) {
    this.log = new SaxtonLogger(this.getClass().getSimpleName(), log);
    this.envMgr = envMgr;
  }

  /**
   * Handle for new route segments.
   * This will help define lane geometries
   * @param currentSeg The current route segment
   */
  public void handleCurrentSegmentMsg(cav_msgs.RouteSegment currentSeg) {
    //TODO update lane geometry here. For now we will need to assume linear interpolation
//    RouteWaypoint wp = RouteWaypoint.fromMessage(currentSeg.getWaypoint());
//    LaneEdgeType leftEdge = wp.getLeftMostLaneMarking();
//    LaneEdgeType interiorEdges = wp.getInteriorLaneMarkings();
//    LaneEdgeType rightEdge = wp.getRightMostLaneMarking();
//    int laneCount = wp.getLaneCount();
//    Location downTrackLoc = wp.getLocation();
//    Location uptrackLoc = RouteWaypoint.fromMessage(currentSeg.getPrevWaypoint()).getLocation();
  }

  /**
   * Handler for new vehicle heading messages
   * Headings should be specified as degrees east of north
   * TODO if base_link frame is not an +X forward frame this will need a transform as well.
   * @param heading The heading message
   */
  public void handleHeadingMsg(cav_msgs.HeadingStamped heading) {
    hostVehicleHeading = heading.getHeading();
    headingReceived = true;
  }

  /**
   * NavSatFix Handler
   * Updates the host vehicle's location and updates the earth->map and map->odom transforms
   * @param navSatFix The nav sat fix message
   */
  public void handleNavSatFixMsg(sensor_msgs.NavSatFix navSatFix) {
    // Assign the new host vehicle location
    hostVehicleLocation =
      new Location(navSatFix.getLatitude(), navSatFix.getLongitude(), navSatFix.getAltitude());
    nabSatFixReceived = true;
    updateMapAndOdomTFs();
  }

  /**
   * Helper function for use in handleNavSatFix
   * Updates the earth->map and map->odom transforms based on the current vehicle odometry, lat/lon, and heading.
   * For full functionality at least one nav sat fix and heading message needs to have been received.
   * Additionally, a transform from base_link to position_sensor needs to be available
   */
  protected void updateMapAndOdomTFs() {
    if (!nabSatFixReceived || !headingReceived) {
      return; // If we don't have a heading and a nav sat fix the map->odom transform cannot be calculated
    }

    // Check if base_link->position_sensor tf is available. If not look it up
    if (baseToPositionSensor == null) {
      // This transform should be static. No need to look up more than once
      baseToPositionSensor = envMgr.getTransform(baseLinkFrame, positionSensorFrame);
      if (baseToPositionSensor == null) {
        return; // If the request for this transform failed wait for another position update to request it
      }
    }

    GeodesicCartesianConverter gcc = new GeodesicCartesianConverter();

    List<geometry_msgs.TransformStamped> tfStampedMsgs = new LinkedList<>();

    // Update map location on start and every MAP_UPDATE_PERIOD after that
    if (prevMapTime == null || 0 < envMgr.getTime().subtract(prevMapTime).compareTo(MAP_UPDATE_PERIOD)) {
      // Map will be an NED frame on the current vehicle location
      earthToMap = gcc.ecefToNEDFromLocaton(hostVehicleLocation);
      tfStampedMsgs.add(buildTFStamped(earthToMap, earthFrame, mapFrame));
      prevMapTime = envMgr.getTime();
    }

    // Calculate map->odom transform
    Point3D hostInMap = gcc.geodesic2Cartesian(hostVehicleLocation, earthToMap.invert());

    // T_x_y = transform describing location of y with respect to x
    // m = map frame
    // p = position sensor frame (from odometry)
    // r = position sensor frame (from nav sat fix)
    // o = odom frame
    // b = baselink frame (as has been calculated by odometry up to this point)
    // We want to find T_p_r. This tells us how much to move odom to correct for drift in odometry
    // T_p_r = inv((inv(T_m_r) * T_m_o * T_o_b * T_b_p); This is equivalent to the difference between where odom should be and where it is
    Vector3 nTranslation = new Vector3(hostInMap.getX(), hostInMap.getY(), hostInMap.getZ());
    // The vehicle heading is relative to NED so over short distances heading in NED = heading in map
    Vector3 zAxis = new Vector3(0,0,1);
    Quaternion hostRotInMap =  Quaternion.fromAxisAngle(zAxis, Math.toRadians(hostVehicleHeading));
    hostRotInMap = hostRotInMap.normalize();

    Transform T_m_r = new Transform(nTranslation, hostRotInMap);
    Transform T_m_o = mapToOdom;
    Transform T_o_b = odomToBaseLink;
    Transform T_b_p = baseToPositionSensor;

    Transform T_p_r = (T_m_r.invert().multiply(T_m_o.multiply(T_o_b.multiply(T_b_p)))).invert();
    Transform tempResult = mapToOdom.multiply(T_p_r);
    Vector3 transResult = tempResult.getTranslation();
    Quaternion quatResult = tempResult.getRotationAndScale();
    // If an NaN transform is calculated do not save or publish it.
    if (Double.isNaN(transResult.getX()) || Double.isNaN(transResult.getY()) || Double.isNaN(transResult.getZ())
      || Double.isNaN(quatResult.getX()) || Double.isNaN(quatResult.getY()) || Double.isNaN(quatResult.getZ())
      || Double.isNaN(quatResult.getW())) {
      log.warn("TRANSFORM", "The mapToOdom transform contains NaN and is being ignored: " + tempResult);
      return;
    }
    // Modify map to odom with the difference from the expected and real sensor positions
    mapToOdom = mapToOdom.multiply(T_p_r);
    // Publish newly calculated transforms
    tfStampedMsgs.add(buildTFStamped(mapToOdom, mapFrame, odomFrame));
    publishTF(tfStampedMsgs);
  }

  /**
   * Odometry message handler
   * @param odometry Odometry message
   */
  public void handleOdometryMsg(nav_msgs.Odometry odometry) {
    // Check if base_link->position_sensor tf is available. If not look it up
    if (baseToPositionSensor == null) {
      // This transform should be static. No need to look up more than once
      baseToPositionSensor = envMgr.getTransform(baseLinkFrame, positionSensorFrame);
      if (baseToPositionSensor == null) {
        return; // If the request for this transform failed wait for another odometry update to request it
      }
    }
    if (odometry.getChildFrameId().equals(baseLinkFrame)) { // If the odometry is already in the base_link frame TODO make this check more robust
      odomToBaseLink = Transform.fromPoseMessage(odometry.getPose().getPose());
      publishTF(Arrays.asList(buildTFStamped(odomToBaseLink, odomFrame, baseLinkFrame)));
      return;
    }
    // Extract the location of the position sensor relative to the odom frame
    // Covariance is ignored as filtering was already done by sensor fusion
    // Calculate odom->base_link
    // T_x_y = transform describing location of y with respect to x
    // p = position sensor frame (from odometry)
    // o = odom frame
    // b = baselink frame (as has been calculated by odometry up to this point)
    // T_o_b = T_o_p * inv(T_b_p)
    Transform T_o_p = Transform.fromPoseMessage(odometry.getPose().getPose());
    Transform T_b_p = baseToPositionSensor;
    Transform T_o_b = T_o_p.multiply(T_b_p.invert());
    odomToBaseLink = T_o_b;
    // Publish updated transform
    publishTF(Arrays.asList(buildTFStamped(odomToBaseLink, odomFrame, baseLinkFrame)));
  }

  /**
   * External object message handler
   * @param externalObjects External object list. Should be relative to base_link frame
   */
  public void handleExternalObjectsMsg(cav_msgs.ExternalObjectList externalObjects) {
    //TODO implement
  }

  /**
   * Velocity message handler
   * @param velocity host vehicle velocity
   */
  public void handleVelocityMsg(geometry_msgs.TwistStamped velocity) {
    //TODO update host vehicle specification
  }

  /**
   * SystemAlert message handler.
   * Will shutdown this node on receipt of FATAL or SHUTDOWN
   * @param alert alert message
   */
  public void handleSystemAlertMsg(cav_msgs.SystemAlert alert) {
    switch (alert.getType()) {
      case SystemAlert.DRIVERS_READY:
        break;
      case SystemAlert.NOT_READY:
        break;
      case SystemAlert.SHUTDOWN:
        log.info("SHUTDOWN", "Received SHUTDOWN on system_alert");
        envMgr.shutdown();
        break;
      case SystemAlert.FATAL:
        log.info("SHUTDOWN", "Received FATAL on system_alert");
        envMgr.shutdown();
        break;
      default:
        // No need to handle other types of alert
    }
  }

  /**
   * Helper function builds a tf2 message for the given transform between parent and child frames
   * @param tf The transform to publish. Describes the position of the child frame in the parent frame
   * @param parentFrame The name of the parent frame
   * @param childFrame The name of the child frame
   */
  protected geometry_msgs.TransformStamped buildTFStamped(Transform tf, String parentFrame, String childFrame) {
    geometry_msgs.TransformStamped tfStampedMsg = messageFactory.newFromType(geometry_msgs.TransformStamped._TYPE);
    Header hdr = tfStampedMsg.getHeader();
    hdr.setFrameId(parentFrame);
    hdr.setStamp(envMgr.getTime());
    hdr.setSeq(tfSequenceCount);
    tfStampedMsg.setChildFrameId(childFrame);
    tfStampedMsg.setTransform(tf.toTransformMessage(tfStampedMsg.getTransform()));
    return tfStampedMsg;
  }

  /**
   * Publishes a list of transforms in a single tf2 TFMessage
   * @param tfList List of transforms
   */
  protected void publishTF(List<TransformStamped> tfList) {
    TFMessage tfMsg = messageFactory.newFromType(TFMessage._TYPE);
    tfMsg.setTransforms(tfList);
    tfSequenceCount++;
    envMgr.publishTF(tfMsg);
  }
}
