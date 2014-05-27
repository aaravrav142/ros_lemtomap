/**
 * @file toponav_map
 * @brief Build and maintain the topological navigation map
 * @author Koen Lekkerkerker
 */

#include <st_topological_mapping/toponav_map.h>

/*!
 * Constructor.
 */
TopoNavMap::TopoNavMap(ros::NodeHandle &n) :
		n_(n), costmap_lastupdate_seq_(0), max_dist_between_nodes_(3.0) // this way, TopoNavMap is aware of the NodeHandle of this ROS node, just as ShowTopoNavMap will be...
{
	ros::NodeHandle private_nh("~");
	std::string scan_topic;
	std::string local_costmap_topic;

	// Parameters initialization
	private_nh.param("scan_topic", scan_topic, std::string("scan"));
	private_nh.param("local_costmap_topic", local_costmap_topic, std::string("/move_base/local_costmap/costmap"));

	local_costmap_sub_ = n_.subscribe(local_costmap_topic, 1, &TopoNavMap::lcostmapCB, this);
	scan_sub_ = n_.subscribe(scan_topic, 1, &TopoNavMap::laserCB, this);
	toponav_map_pub_ = private_nh.advertise<
			st_topological_mapping::TopologicalNavigationMap>(
			"topological_navigation_map", 1,true);

	updateMap(); //update the map one time, at construction. This will create the first map node.

#if DEBUG
	test_executed_ = 0;
	last_run_update_max_=0;
	initialpose_sub_ = n_.subscribe("initialpose", 10, &TopoNavMap::initialposeCB, this);
#endif
}

TopoNavMap::~TopoNavMap() {
	while (edges_.size() > 0) {
		delete edges_.rbegin()->second;
	}
	while (nodes_.size() > 0) {
		delete nodes_.rbegin()->second;
	}
	std::cerr
			<< "~TopoNavMap: Deleting object -> all TopoNavNodes and TopoNavEdges are destructed"
			<< std::endl;
}

/*!
 * Laser Callback. Update laser_scan_
 */
void TopoNavMap::laserCB(const sensor_msgs::LaserScan::ConstPtr &msg) {
	ROS_DEBUG("LaserCallback");
	laser_scan_ = *msg;
	ROS_DEBUG("angle_max=%f", laser_scan_.angle_max); // to check whether it uses kinect vs hokuyo
}

/*!
 * Local Costmap Callback. Update laser_scan_
 */
void TopoNavMap::lcostmapCB(const nav_msgs::OccupancyGrid::ConstPtr &msg){

#if DEBUG
	/*ROS_INFO("Last lcostmapCB cycle took %.4f seconds",(ros::Time::now()-last_run_lcostmap_).toSec());
	last_run_lcostmap_ = ros::Time::now();*/
#endif

	ROS_DEBUG("Local Costmap Callback");
	local_costmap_ = *msg;
	poseMsgToTF(local_costmap_.info.origin,local_costmap_origin_tf_);
	br_.sendTransform(tf::StampedTransform(local_costmap_origin_tf_, ros::Time::now(), local_costmap_.header.frame_id, "local_costmap_origin"));
}

#if DEBUG
void TopoNavMap::initialposeCB(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr &msg){
initialpose_.header = msg->header;
initialpose_.pose = msg->pose.pose;
tf::Point point1, point2;
pointMsgToTF(initialpose_.pose.position,point1);

directNavigable(point1, point2);
}
#endif

/*!
 * Publish the Topological Navigation Map.
 */
void TopoNavMap::publishTopoNavMap() {
	ROS_DEBUG("publishTopoNavMap");
	st_topological_mapping::TopologicalNavigationMap msg_map;

	for (TopoNavNode::NodeMap::iterator it=nodes_.begin(); it!=nodes_.end(); it++) {
		msg_map.nodes.push_back(nodeToRosMsg(it->second));
	}
	for (TopoNavEdge::EdgeMap::iterator it=edges_.begin(); it!=edges_.end(); it++) {
		msg_map.edges.push_back(edgeToRosMsg(it->second));
	}

	toponav_map_pub_.publish(msg_map);
}

/*!
 * getCurrentPose
 */
void TopoNavMap::getCurrentPose() {
	try {
		tf_listener_.waitForTransform("/map", "/base_link", ros::Time(0),
				ros::Duration(10));
		tf_listener_.lookupTransform("/map", "/base_link", ros::Time(0),
				robot_transform_tf_);
	} catch (tf::TransformException &ex) {
		ROS_ERROR("Error looking up transformation\n%s", ex.what());
	}

	robot_pose_tf_.setOrigin(robot_transform_tf_.getOrigin());
	robot_pose_tf_.setRotation(robot_transform_tf_.getRotation());

	ROS_DEBUG("Pose is x=%f, y=%f, theta=%f", robot_pose_tf_.getOrigin().x(),
			robot_pose_tf_.getOrigin().y(),
			tf::getYaw(robot_pose_tf_.getRotation()));
}

/*!
 * loadMapFromMsg
 */
void TopoNavMap::loadMapFromMsg(
		const st_topological_mapping::TopologicalNavigationMap &toponavmap_msg) {
	nodes_.clear();
	edges_.clear();

	for (int i = 0; i < toponavmap_msg.nodes.size(); i++) {
		nodeFromRosMsg(toponavmap_msg.nodes.at(i), nodes_);
		ROS_DEBUG("Loaded node with ID=%lu to std::map nodes_", toponavmap_msg.nodes.at(i).node_id);
	}
	for (int i = 0; i < toponavmap_msg.edges.size(); i++) {
		new TopoNavEdge(
				toponavmap_msg.edges.at(i).edge_id, //edge_id
				toponavmap_msg.edges.at(i).last_updated, //last_updated
				toponavmap_msg.edges.at(i).cost, //cost
				*nodes_[toponavmap_msg.edges.at(i).start_node_id],
				*nodes_[toponavmap_msg.edges.at(i).end_node_id],
				edges_ //edges std::map
				);
		ROS_DEBUG("Loaded edge with ID=%lu to std::map edges_", toponavmap_msg.nodes.at(i).node_id);
	}
	ROS_INFO("Finished loading the TopoNavMap");
}

/*!
 * updateMap
 */
void TopoNavMap::updateMap() {

#if DEBUG
	/*
	ROS_INFO("Last updateMap cycle took %.4f seconds",(ros::Time::now()-last_run_update_).toSec());
	last_run_update_ = ros::Time::now();
	if ((ros::Time::now()-last_run_update_).toSec() > last_run_update_max_)
		last_run_update_max_ = (ros::Time::now()-last_run_update_).toSec();
	*/
#endif

	getCurrentPose();

	checkCreateNode();

	publishTopoNavMap();

#if DEBUG

	 /*if (ros::Time().now()>ros::Time(35) && test_executed_==0)
	 { //this code is to test stuff timed...
	 ROS_INFO("Deleting node 6");
	 deleteNode(6);
	 test_executed_++;
	 }
	 if (ros::Time().now()>ros::Time(37) && test_executed_==1)
	 { //this code is to test stuff timed...
	 ROS_INFO("Deleting node 2");
	 deleteNode(2);
	 test_executed_++;
	 }

	 if (ros::Time().now()>ros::Time(39) && test_executed_==2)
	 { //this code is to test stuff timed...
	 ROS_INFO("Moving node 4");
	 tf::Pose tmp_pose=nodes_[4]->getPose();
	 tmp_pose.getOrigin().setY(tmp_pose.getOrigin().getY()+0.3);
	 nodes_[4]->setPose(tmp_pose);
	 test_executed_++;
	 }*/

#endif
}

/*!
 * checkCreateNewNode
 */
bool TopoNavMap::checkCreateNode() {
	int number_of_nodes = getNumberOfNodes();
	int area_id = 1; //FIXME: room_id is always 1!
	bool create_node = false;
	bool is_door = false;

	if (checkIsNewDoor()) {
		//TODO: later, maybe door nodes should not influence other nodes. Maybe they should not be regular nodes at all. Check SAS10 for comparison.
		create_node = true;
		is_door = true;
	} else if (distanceToClosestNode() > 1) {
		//TODO FIXME: Remove magic number "1", which is the min distance here...
		create_node = true;
	}
	if (create_node) {
		addNode(robot_pose_tf_, is_door, area_id);
		checkCreateEdges((*nodes_.rbegin()->second)); //(*nodes_.rbegin()->second) should pass it the node that was just created...

		return true;
	} else {
		ROS_DEBUG("No new node created");
		return false;
	}
}

/*!
 * checkCreateEdges
 */
bool TopoNavMap::checkCreateEdges(const TopoNavNode &node) {
	//@TODO This method compares with all nodes: does not scale very well.
	bool edge_created = false;
	if (getNumberOfNodes() < 2) return false; //only continue if there are 2 or more nodes

	for (TopoNavNode::NodeMap::iterator it=nodes_.begin(); it!=nodes_.end(); it++) {
		if (it->second->getNodeID() == node.getNodeID())
			continue; //Not compare with itself
		if (calcDistance(node, *it->second) > max_dist_between_nodes_)
			continue; //Only if it is close enough
		if (!edgeExists(node, *(it->second)) && directNavigable(node.getPose().getOrigin(), it->second->getPose().getOrigin())) {
			addEdge(node, *(it->second));
			edge_created = true;
		}
	}
	ROS_DEBUG_COND(!edge_created,
			"During this 'checkCreateEdges' call, no edge was created.");
	return edge_created;

}

/*!
 * directNavigable
 */
const bool TopoNavMap::directNavigable(const tf::Point &point1,
		const tf::Point &point2) {
	bool navigable = false;

	//Check if the local_costmap has changed since last run, if so, update it
	if (local_costmap_.header.seq > costmap_lastupdate_seq_){ //only update the matrix if a newer version of the costmap has been published
		//set lastupdate to the current message
		costmap_lastupdate_seq_ = local_costmap_.header.seq;

		int costmap_height_c, costmap_width_c; //_c for cells

		costmap_height_c = local_costmap_.info.height;
		costmap_width_c = local_costmap_.info.width;

		// Create the costmap as a matrix. Cost goes from 0 (free), to 100 (obstacle). -1 would be uknown, but I think it is not used for costmaps, only normal gridmaps...
		costmap_matrix_.resize(costmap_height_c,costmap_width_c);
		for(int i = 0 ; i < costmap_height_c ; i++){
			for(int j = 0 ; j < costmap_width_c ; j++){
				costmap_matrix_(i,j)=local_costmap_.data[i*costmap_width_c+j];
			}
		}
	}

	int i_cell,j_cell;
	mapPoint2costmapCell(point1,i_cell,j_cell);
	double cost = costmap_matrix_(i_cell,j_cell);
	ROS_INFO("You clicked at: x_cell=%d, y_cell=%d, cost=%.4f",i_cell,j_cell,cost);
	//getCMLineCost(i1_cell,i1_cell,j2_cell,j2_cell);
	//getCMLineCost(point1,point2);

	return navigable;
}

/**\brief turn a /map tf::Point into a cell of the local costmap
 * \param map_coordinate The coordiante in the /map
 * \param cell_x (output) The girdcell coordinate x
 * \param cell_y (output) The girdcell coordinate y
 * \return N/A
 */
void TopoNavMap::mapPoint2costmapCell(const tf::Point &map_coordinate, int &cell_i, int &cell_j) const {
	// Calculate a Transform from map coordinates to costmap origin coordinates (which is at costmap_matrix_(0,0))
	std::string costmap_frame_id; //global frame as specified in move_base local costmap params
	costmap_frame_id = local_costmap_.header.frame_id;

	tf::Stamped<tf::Point> map_coordinate_stamped(map_coordinate,ros::Time(0),"/map");
	tf::Stamped<tf::Point> map_coordinate_incostmap_origin;
	try {
		//listener_.waitForTransform("local_costmap_origin", "/map", ros::Time(0), ros::Duration(1.0)); //not necessary
		listener_.transformPoint("local_costmap_origin",map_coordinate_stamped,map_coordinate_incostmap_origin);
	}
	catch (tf::TransformException &ex) {
		ROS_ERROR("%s",ex.what());
	}
	ROS_INFO("map_coordinate_incostmap_origin x=%.4f , y=%.4f", map_coordinate_incostmap_origin.getX(), map_coordinate_incostmap_origin.getY());

	cell_i = floor(map_coordinate_incostmap_origin.getY()/local_costmap_.info.resolution); // i is row, i.e. y
	cell_j = floor(map_coordinate_incostmap_origin.getX()/local_costmap_.info.resolution); // j is column, i.e. x

	if (cell_i < 0 || cell_j < 0 || cell_i >= local_costmap_.info.height || cell_j >= local_costmap_.info.width)
		ROS_ERROR("mapPoint2costmapCell: Index out of bounds!");
}


/*!
 * edgeExists
 */
const bool TopoNavMap::edgeExists(const TopoNavNode &node1,
		const TopoNavNode &node2) const {
	//TODO: if giving the edges and ID like the string "2to1", you will have unique IDs that are descriptive enough to facilitate edgeExists etc.
	ROS_WARN_ONCE(
			"edgeExists is not yet implemented. It should help block recreation of edges in checkCreateEdge. This goes well for new edges (there is no risk of duplicates), but triggering checkCreateEdge when updating a node for example will likely lead to duplicate edges. This message will only print once.");
	return false;
}

/*!
 * checkIsDoor
 */
bool TopoNavMap::checkIsNewDoor() {
//@TODO write this method
	ROS_WARN_ONCE(
			"Detecting/creating Doors is not yet implemented. This message will only print once.");
	return false;
}

/*!
 * distanceToClosestNode
 */
double TopoNavMap::distanceToClosestNode() {
// @TODO: This method compares to all nodes -> scales poorly eventually!
// One idea to make it scale slightly better:bool anyNodeCloserThen(max_dist), which return false if there isnt any (full search space needs to be searched) or returns true if there is (usually only first part of search space needs to be searched, if you start at end of nodes_ std::map)
	double dist, minimum_dist;
	int closest_node_id;
	int number_of_nodes = getNumberOfNodes();
	if (number_of_nodes == 0)
		minimum_dist = INFINITY; //No nodes means -> dist in inf.
	else {
		for (TopoNavNode::NodeMap::iterator it=nodes_.begin(); it!=nodes_.end(); it++) {
			dist = calcDistance(*(it->second), robot_pose_tf_);

			ROS_DEBUG("Distance between Robot and Node_ID %d = %f",
					it->second->getNodeID(), dist);

			if (it == nodes_.begin() || dist < minimum_dist) {
				minimum_dist = dist;
				closest_node_id = it->second->getNodeID();
			}
		}
	}
	ROS_DEBUG("Minimum distance = [%f], Closest Node ID= [%d]", minimum_dist,
			closest_node_id);

	return minimum_dist;

}

/*!
 * addEdge
 */
void TopoNavMap::addEdge(const TopoNavNode &start_node,
		const TopoNavNode &end_node) {
	new TopoNavEdge(start_node, end_node, edges_); //Using "new", the object will not be destructed after leaving this method!
}

/*!
 * addNode
 */
void TopoNavMap::addNode(const tf::Pose &pose, bool is_door, int area_id) {
	new TopoNavNode(pose, is_door, area_id, nodes_); //Using "new", the object will not be destructed after leaving this method!
}

/*!
 * deleteEdge
 */
void TopoNavMap::deleteEdge(TopoNavEdge::EdgeID edge_id) {
	deleteEdge(*edges_[edge_id]);
}
void TopoNavMap::deleteEdge(TopoNavEdge &edge) {
	delete &edge;
}

/*!
 * deleteNode
 */
void TopoNavMap::deleteNode(TopoNavNode::NodeID node_id) {
	deleteNode(*nodes_[node_id]);
}
void TopoNavMap::deleteNode(TopoNavNode &node) {
	TopoNavEdge::EdgeMap connected_edges = connectedEdges(node);
	for (TopoNavEdge::EdgeMap::iterator it=connected_edges.begin(); it!=connected_edges.end(); it++) {
		deleteEdge((*it->second));
	}
	delete &node;
}

TopoNavEdge::EdgeMap TopoNavMap::connectedEdges(
		const TopoNavNode &node) const { //TODO scales poorly: all edges are checked!
	TopoNavEdge::EdgeMap connected_edges;
	for (TopoNavEdge::EdgeMap::const_iterator it=edges_.begin(); it!=edges_.end(); it++) {
		if (it->second->getStartNode().getNodeID() == node.getNodeID()
				|| it->second->getEndNode().getNodeID() == node.getNodeID()) {
			connected_edges[it->second->getEdgeID()]=(it->second);
		}
	}
	return connected_edges;
}


void TopoNavMap::nodeFromRosMsg(const st_topological_mapping::TopoNavNodeMsg node_msg, TopoNavNode::NodeMap &nodes) {
	tf::Pose tfpose;
	poseMsgToTF(node_msg.pose,tfpose);

	new TopoNavNode(
			node_msg.node_id, //node_id
			node_msg.last_updated,//last_updated
			tfpose,//pose
			node_msg.is_door,//is_door
			node_msg.area_id,//area_id
			nodes//nodes map
	);
}

void TopoNavMap::edgeFromRosMsg(const st_topological_mapping::TopoNavEdgeMsg edge_msg, TopoNavEdge::EdgeMap &edges) {
	new TopoNavEdge(edge_msg.edge_id, //edge_id
			edge_msg.last_updated, //last_updated
			edge_msg.cost, //cost
			*nodes_[edge_msg.start_node_id], //start_node
			*nodes_[edge_msg.end_node_id], //end_node
			edges //edges std::map
			);
}
st_topological_mapping::TopoNavEdgeMsg TopoNavMap::edgeToRosMsg(const TopoNavEdge* edge){
	st_topological_mapping::TopoNavEdgeMsg msg_edge;
	msg_edge.edge_id = edge->getEdgeID();
	msg_edge.last_updated = edge->getLastUpdatedTime();
	msg_edge.start_node_id = edge->getStartNode().getNodeID();
	msg_edge.end_node_id = edge->getEndNode().getNodeID();
	msg_edge.cost = edge->getCost();

	return msg_edge;
}

st_topological_mapping::TopoNavNodeMsg TopoNavMap::nodeToRosMsg(
		const TopoNavNode* node) {
	st_topological_mapping::TopoNavNodeMsg msg_node;
	msg_node.node_id = node->getNodeID();
	msg_node.last_updated = node->getLastUpdatedTime();
	msg_node.area_id = node->getAreaID();
	poseTFToMsg(node->getPose(), msg_node.pose);
	msg_node.is_door = node->getIsDoor();

	return msg_node;
}
