/*
 * Copyright 2016 Fixstars Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdexcept>
#include <thread>
#include <atomic>
#include "system/topology.hpp"
#include "logging/general_logger.hpp"

namespace m3bp {
namespace {

#ifdef M3BP_LOCALITY_ENABLED
thread_local identifier_type g_binded_node = 0;

hwloc_topology_t initialize_topology(){
	hwloc_topology_t topology;
	if(hwloc_topology_init(&topology) != 0){
		throw std::runtime_error(
			"An error occured on `hwloc_topology_init()`");
	}
	if(hwloc_topology_load(topology) != 0){
		throw std::runtime_error(
			"An error occured on `hwloc_topology_load()`");
	}
	return topology;
}

std::vector<identifier_type>
enumerate_processing_units(hwloc_topology_t topology){
	const auto n = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
	if(n == -1){
		throw std::runtime_error(
			"An error occured on `hwloc_get_nbobjs_by_type(HWLOC_OBJ_PU)`");
	}

	hwloc_cpuset_t before_cpuset = hwloc_bitmap_alloc();
	hwloc_get_cpubind(topology, before_cpuset, HWLOC_CPUBIND_THREAD);

	std::vector<identifier_type> available_units;
	for(int i = 0; i < n; ++i){
		const auto obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_PU, i);
		if(hwloc_set_cpubind(topology, obj->cpuset, HWLOC_CPUBIND_THREAD) == 0){
			available_units.push_back(i);
		}
	}

	hwloc_set_cpubind(topology, before_cpuset, HWLOC_CPUBIND_THREAD);
	hwloc_bitmap_free(before_cpuset);
	return available_units;
}

std::vector<identifier_type>
enumerate_numa_nodes(hwloc_topology_t topology){
	if(hwloc_get_type_depth(topology, HWLOC_OBJ_NODE) <= 0){
		// Current system topology does not have the depth of NUMA nodes
		return std::vector<identifier_type>(1, 0);
	}
	const auto n = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_NODE);
	if(n == -1){
		throw std::runtime_error(
			"An error occured on `hwloc_get_nbobjs_by_type(HWLOC_OBJ_NODE)`");
	}

	hwloc_cpuset_t before_cpuset = hwloc_bitmap_alloc();
	hwloc_get_cpubind(topology, before_cpuset, HWLOC_CPUBIND_THREAD);

	std::vector<identifier_type> available_nodes;
	for(int i = 0; i < n; ++i){
		const auto obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_NODE, i);
		if(hwloc_set_cpubind(topology, obj->cpuset, HWLOC_CPUBIND_THREAD) == 0){
			available_nodes.push_back(i);
		}
	}

	hwloc_set_cpubind(topology, before_cpuset, HWLOC_CPUBIND_THREAD);
	hwloc_bitmap_free(before_cpuset);
	return available_nodes;
}
#endif

}

#ifdef M3BP_LOCALITY_ENABLED

Topology::Topology()
	: m_topology(initialize_topology())
	, m_available_processing_units(enumerate_processing_units(m_topology))
	, m_available_numa_nodes(enumerate_numa_nodes(m_topology))
	, m_processing_units_per_node(m_available_numa_nodes.size())
{
	const auto num_nodes = m_available_numa_nodes.size();
	for(const auto pu : m_available_processing_units){
		if(num_nodes == 1){
			++m_processing_units_per_node[0];
		}else{
			auto obj = hwloc_get_obj_by_type(m_topology, HWLOC_OBJ_PU, pu);
			while(obj && obj->type != HWLOC_OBJ_NODE){ obj = obj->parent; }
			if(obj && obj->type == HWLOC_OBJ_NODE){
				const auto it = std::find(
					m_available_numa_nodes.begin(),
					m_available_numa_nodes.end(),
					obj->logical_index);
				const auto index = it - m_available_numa_nodes.begin();
				++m_processing_units_per_node[index];
			}else{
				++m_processing_units_per_node[0];
			}
		}
	}
}

Topology::~Topology(){
	hwloc_topology_destroy(m_topology);
}

#else

Topology::Topology()
	: m_available_processing_units(std::thread::hardware_concurrency())
	, m_available_numa_nodes(1, 0)
	, m_processing_units_per_node(1, m_available_processing_units.size())
{
	const auto n = m_available_processing_units.size();
	for(identifier_type i = 0; i < n; ++i){
		m_available_processing_units[i] = i;
	}
}

Topology::~Topology() = default;

#endif

Topology &Topology::instance(){
	static Topology s_instance;
	return s_instance;
}


void Topology::set_thread_cpubind(identifier_type numa_node){
#ifdef M3BP_LOCALITY_ENABLED
	const auto num_nodes = m_available_numa_nodes.size();
	assert(numa_node < num_nodes);
	hwloc_obj_t obj;
	// What's the difference between HWLOC_OBJ_MACHINE and HWLOC_OBJ_NODE ?
	// https://www.open-mpi.org/projects/hwloc/doc/v1.3.1/a00040.php#gacd37bb612667dc437d66bfb175a8dc55
	// http://www.training.prace-ri.eu/uploads/tx_pracetmo/affinity.pdf
	if(num_nodes == 1){
		obj = hwloc_get_obj_by_type(m_topology, HWLOC_OBJ_MACHINE, 0);
		if(!obj){
			throw std::runtime_error(
				"An error occured on "
				"`hwloc_get_obj_by_type(HWLOC_OBJ_MACHINE)`");
		}
	}else{
		// Get the numa nodes with the index m_available_numa_nodes[numa_node].
		obj = hwloc_get_obj_by_type(
			m_topology, HWLOC_OBJ_NODE, m_available_numa_nodes[numa_node]);
		if(!obj){
			throw std::runtime_error(
				"An error occured on `hwloc_get_obj_by_type(HWLOC_OBJ_NODE)`");
		}
	}
	// Bind current thread to the numa node.
	if(hwloc_set_cpubind(m_topology, obj->cpuset, HWLOC_CPUBIND_THREAD) != 0){
		throw std::runtime_error("An error occured on `hwloc_set_cpubind()`");
	}
	g_binded_node = numa_node;
#endif
}


void *Topology::allocate_membind(size_type size){
	return allocate_membind(size, g_binded_node);
}

void *Topology::allocate_membind(
	size_type size, identifier_type numa_node)
{
	// what is this? First touch?
	(void)(numa_node);
	assert(numa_node < m_processing_units_per_node.size());
	void *ptr = malloc(size);
	if(!ptr){ throw std::bad_alloc(); }
	return ptr;
}

void Topology::release_membind(
	void *p, size_type /* size */) noexcept
{
	free(p);
}

}

