/*
 * Parser.cpp
 *
 *  Created on: May 9, 2020
 *      Author: teng
 */

#include "../geometry/MyPolygon.h"
#include <fstream>
#include "../index/RTree.h"
#include <queue>
#include <boost/program_options.hpp>


namespace po = boost::program_options;
using namespace std;

#define MAX_THREAD_NUM 100

int element_size = 100;

// some shared parameters
pthread_mutex_t poly_lock;
bool stop = false;

pthread_mutex_t report_lock;

double *points;
size_t cur_index = 0;
size_t total_points = 0;

query_context global_ctx;

RTree<MyPolygon *, double, 2, double> tree;
vector<MyPolygon *> source;
int source_index = 0;
int partition_count = 0;



int batch_num = 100;
void *partition_unit(void *args){
	query_context *ctx = (query_context *)args;
	//log("thread %d is started",ctx->thread_id);
	int local_count = 0;
	while(source_index!=source.size()){
		int local_cur = 0;
		int local_end = 0;
		pthread_mutex_lock(&poly_lock);
		if(source_index==source.size()){
			pthread_mutex_unlock(&poly_lock);
			break;
		}
		local_cur = source_index;
		if(local_cur+1>source.size()){
			local_end = source.size();
		}else {
			local_end = local_cur+1;
		}
		source_index = local_end;
		pthread_mutex_unlock(&poly_lock);

		for(int i=local_cur;i<local_end;i++){
			struct timeval start = get_cur_time();

			if(ctx->use_grid){
				source[i]->partition(ctx->vpr);
				ctx->partitions_count += source[i]->get_num_partitions();
				//log("%d %d",source[i]->get_num_vertices(),source[i]->get_num_partitions());
			}
			if(ctx->use_qtree){
				QTNode *qtree = source[i]->partition_qtree(ctx->vpr);
				ctx->total_partition_size += qtree->size();
				ctx->partitions_count += qtree->leaf_count();
			}
			double latency = get_time_elapsed(start);
			int num_vertices = source[i]->get_num_vertices();
			ctx->report_latency(num_vertices, latency);
			if(latency>10000||num_vertices>200000){
				logt("partition %d vertices",start,num_vertices);
			}
//			if(num_vertices>400000){
//				cout<<source[i]->to_string()<<endl;
//			}

			ctx->total_data_size += source[i]->get_data_size();

			if(++local_count==1000){
				pthread_mutex_lock(&report_lock);
				partition_count += local_count;
				if(partition_count%10000==0){
					log("partitioned %d polygons",partition_count);
				}
				local_count = 0;
				pthread_mutex_unlock(&report_lock);
			}
		}
	}
	pthread_mutex_lock(&report_lock);
	partition_count += local_count;
	global_ctx = global_ctx + *ctx;
	pthread_mutex_unlock(&report_lock);

	return NULL;
}


bool MySearchCallback(MyPolygon *poly, void* arg){
	query_context *ctx = (query_context *)arg;
	// query with parition
	if(ctx->use_grid){
		assert(poly->is_grid_partitioned());
		ctx->found += poly->contain(ctx->target_p,ctx);
		if(ctx->rastor_only){
			ctx->raster_checked++;
		}else{
			ctx->vector_check++;
		}
	}else if(ctx->use_qtree){
		assert(poly->is_qtree_partitioned());
		QTNode *tnode = poly->get_qtree()->retrieve(ctx->target_p);
		if(tnode->exterior||tnode->interior){
			ctx->found += tnode->interior;
			ctx->raster_checked++;
		}else if(ctx->query_vector){
			ctx->found += poly->contain(ctx->target_p,ctx);
			ctx->vector_check++;
		}
	}else{
		ctx->found += poly->contain(ctx->target_p,ctx);
		ctx->vector_check++;
	}
	return true;
}

void *query(void *args){
	query_context *ctx = (query_context *)args;
	//log("thread %d is started",ctx->thread_id);
	while(cur_index!=total_points){
		int local_cur = 0;
		int local_end = 0;
		pthread_mutex_lock(&poly_lock);
		if(cur_index==total_points){
			pthread_mutex_unlock(&poly_lock);
			break;
		}
		local_cur = cur_index;
		if(local_cur+batch_num>total_points){
			local_end = total_points;
		}else {
			local_end = local_cur+batch_num;
		}
		cur_index = local_end;
		pthread_mutex_unlock(&poly_lock);

		for(int i=local_cur;i<local_end;i++){

			if(ctx->sample_rate<1.0&&!tryluck(ctx->sample_rate)){
				continue;
			}

			ctx->target_p = Point(points[2*i],points[2*i+1]);
			tree.Search(points+2*i, points+2*i, MySearchCallback, (void *)ctx);
			if(++ctx->query_count==1000){
				pthread_mutex_lock(&report_lock);
				global_ctx.query_count += ctx->query_count;
				if(global_ctx.query_count%1000000==0){
					log("queried %d points",global_ctx.query_count);
				}
				ctx->query_count = 0;
				pthread_mutex_unlock(&report_lock);
			}
		}
	}
	pthread_mutex_lock(&report_lock);
	global_ctx = global_ctx+*ctx;
	pthread_mutex_unlock(&report_lock);

	return NULL;
}



int main(int argc, char** argv) {
	string source_path;
	string target_path;
	int num_threads = get_num_threads();
	po::options_description desc("query usage");
	desc.add_options()
		("help,h", "produce help message")
		("partition_only", "partition only")
		("rasterize,r", "partition with rasterization")
		("qtree,q", "partition with qtree")
		("source,s", po::value<string>(&source_path), "path to the source")
		("target,t", po::value<string>(&target_path), "path to the target")
		("threads,n", po::value<int>(&num_threads), "number of threads")
		("vpr,v", po::value<int>(&global_ctx.vpr), "number of vertices per raster")
		("big_threshold,b", po::value<int>(&global_ctx.big_threshold), "up threshold for complex polygon")
		("small_threshold", po::value<int>(&global_ctx.small_threshold), "low threshold for complex polygon")
		("sample_rate", po::value<float>(&global_ctx.sample_rate), "sample rate")
		("raster_only", "query with raster only")
		;
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	if (vm.count("help")) {
		cout << desc << "\n";
		return 0;
	}
	po::notify(vm);

	if(!vm.count("source")||!vm.count("target")){
		cout << desc << "\n";
		return 0;
	}
	global_ctx.use_grid = vm.count("rasterize");
	global_ctx.use_qtree = vm.count("qtree");
	global_ctx.query_vector = !vm.count("raster_only");

	assert(!(global_ctx.use_grid&&global_ctx.use_qtree));
	timeval start = get_cur_time();

	global_ctx.sort_polygons = true;
	source = MyPolygon::load_binary_file(source_path.c_str(),global_ctx);
	logt("loaded %ld polygons", start, source.size());

	pthread_t threads[num_threads];
	query_context ctx[num_threads];
	for(int i=0;i<num_threads;i++){
		ctx[i] = global_ctx;
		ctx[i].thread_id = i;
	}

	if(vm.count("rasterize")||vm.count("qtree")){
		for(int i=0;i<num_threads;i++){
			pthread_create(&threads[i], NULL, partition_unit, (void *)&ctx[i]);
		}

		for(int i = 0; i < num_threads; i++ ){
			void *status;
			pthread_join(threads[i], &status);
		}
		logt("partitioned %d polygons with %ld average partitions", start, partition_count, global_ctx.partitions_count/source.size());
	}


	if(vm.count("raster_only")){
//		for(auto it:global_ctx.partition_vertex_number){
//			cout<<it.first<<"\t"<<global_ctx.partition_latency[it.first]/it.second<<endl;
//		}
		//source[0]->print_partition(global_ctx);
		log("%ld %ld",global_ctx.total_data_size,global_ctx.total_partition_size);
		return 0;
	}



	int treesize = 0;
	for(MyPolygon *p:source){
		tree.Insert(p->getMBB()->low, p->getMBB()->high, p);
		treesize++;
	}
	logt("building R-Tree with %d nodes", start, treesize);

	// read all the points
	long fsize = file_size(target_path.c_str());
	if(fsize<=0){
		log("%s is empty",target_path.c_str());
		exit(0);
	}else{
		log("size of %s is %ld", target_path.c_str(),fsize);
	}
	total_points = fsize/(2*sizeof(double));

	points = new double[total_points*2];
	ifstream infile(target_path.c_str(), ios::in | ios::binary);
	infile.read((char *)points, fsize);
	logt("loaded %ld points", start,total_points);



	for(int i=0;i<num_threads;i++){
		pthread_create(&threads[i], NULL, query, (void *)&ctx[i]);
	}

	for(int i = 0; i < num_threads; i++ ){
		void *status;
		pthread_join(threads[i], &status);
	}
	logt("queried %d polygons %ld rastor %ld vector %ld edges per vector %ld found",start,global_ctx.query_count,global_ctx.raster_checked,global_ctx.vector_check
			,global_ctx.vector_check==0?0:global_ctx.edges_checked/global_ctx.vector_check, global_ctx.found);
	for(MyPolygon *p:source){
		delete p;
	}



	return 0;
}


