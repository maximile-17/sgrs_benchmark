/*
 * simple bench with 2 procs:copy and NIC DMA overhead 
 */
#include "sgrs.h"

int main(int argc, char **argv)
{
	int myrank, numprocs;
	int opt;

	MPI_Init(&argc, &argv);

	MPI_Comm_size(MPI_COMM_WORLD, &numprocs);
	MPI_Comm_rank(MPI_COMM_WORLD, &myrank);



	// setup data layout
	struct params parameters;
	parameters.block_size =  BLOCK_SZ;
 	parameters.block_num = BLOCK_N;
	parameters.stride = STRIDE;
  	parameters.iterW = WITER;
  	parameters.iterN = NITER;
	parameters.IBlink = 1;
	parameters.Ethlink = 0;  
	parameters.mtu = 1024;

	if (parser(argc, argv, &parameters, numprocs, myrank)) {
		if (0 == myrank) {
			fprintf(stderr, "Parser parameters error!\n");
		}
		goto EXIT_MPI_FINALIZE;
	}

	// initialize random number generator
	srand48(getpid()*time(NULL));

	// find IB devices
	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		fprintf(stderr, "failed to get device list!\n");
		goto EXIT_MPI_FINALIZE;
	}

	// pick the HCA
	dev = dev_list[IB_DEV];
	dev_ctx = ibv_open_device(dev);
	if (!dev_ctx) {
		fprintf(stderr, "failed to open device!\n");
		goto EXIT_FREE_DEV_LIST;
	}
	if (ibv_query_device(dev_ctx, &dev_attr)) {
		fprintf(stderr, "failed to query device!\n");
		goto EXIT_CLOSE_DEV;
	}
	if (parameters.block_num > dev_attr.max_sge) {
		parameters.block_num = dev_attr.max_sge;
		if(myrank == 0)
		printf("block_num is bigger than the device's max_sge,reset equal to max_sge:%d\n",dev_attr.max_sge);
	}
	// check the port
	if (ibv_query_port(dev_ctx, IB_PORT, &port_attr)) {
		fprintf(stderr, "failed to query port!\n");
		goto EXIT_CLOSE_DEV;
	}
	if (port_attr.state != IBV_PORT_ACTIVE) {
		fprintf(stderr, "port not ready!\n");
		goto EXIT_CLOSE_DEV;
	}

	// print some device info
	printf("rank%d | device: %s | port: %d | lid: %u | max sge: %d\n",
	        myrank, ibv_get_device_name(dev), IB_PORT, port_attr.lid, dev_attr.max_sge);

	// allocate protection domain
	pd = ibv_alloc_pd(dev_ctx);
	if (!pd) {
		fprintf(stderr, "failed to allocate protection domain!\n");
		goto EXIT_CLOSE_DEV;
	}

	// allocate memory buffers
	buf_size = parameters.stride * dev_attr.max_sge;
	if (posix_memalign(&buf_sg, sysconf(_SC_PAGESIZE), buf_size)) {
		fprintf(stderr, "failed to allocate S/G buffer!\n");
		goto EXIT_DEALLOC_PD;
	}
	if (posix_memalign(&buf_cp, sysconf(_SC_PAGESIZE), buf_size)) {
		fprintf(stderr, "failed to allocate copy buffer!\n");
		goto EXIT_FREE_BUF;
	}

	// register the S/G buffer for RDMA
	mr = ibv_reg_mr(pd, buf_sg, buf_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!mr) {
		fprintf(stderr, "failed to set up MR!\n");
		goto EXIT_FREE_BUF;
	}

	// create the completion queue
	cq = ibv_create_cq(dev_ctx, CQ_DEPTH, NULL, NULL, 0);
	if (!cq) {
		fprintf(stderr, "failed to create receive CQ!\n");
		goto EXIT_DEREG_MR;
	}

	// create the queue pair
	qp_init_attr.qp_type             = IBV_QPT_RC;
	qp_init_attr.send_cq             = cq;    // NULL would cause segfault ...
	qp_init_attr.recv_cq             = cq;
	qp_init_attr.cap.max_send_wr     = CQ_DEPTH;
	qp_init_attr.cap.max_recv_wr     = CQ_DEPTH;
	qp_init_attr.cap.max_send_sge    = dev_attr.max_sge;
	qp_init_attr.cap.max_recv_sge    = dev_attr.max_sge;
	qp = ibv_create_qp(pd, &qp_init_attr);
	if (!qp) {
		fprintf(stderr, "failed to create QP!\n");
		goto EXIT_DESTROY_CQ;
	}

	// set QP status to INIT
	qp_attr.qp_state        = IBV_QPS_INIT;
	qp_attr.pkey_index      = 0;
	qp_attr.port_num        = IB_PORT;
	qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
	if (ibv_modify_qp(qp, &qp_attr,
	                       IBV_QP_STATE      |
	                       IBV_QP_PKEY_INDEX |
	                       IBV_QP_PORT       |
	                       IBV_QP_ACCESS_FLAGS)) {
		fprintf(stderr, "failed to modify qp state to INIT!\n");
		goto EXIT_DESTROY_QP;
	};

	// exchange connection info
	local_conn  = (const struct ib_conn) { 0 };
	remote_conn = (const struct ib_conn) { 0 };
	local_conn.lid = port_attr.lid;
	local_conn.qpn = qp->qp_num;
	local_conn.psn = lrand48() & 0xffffff;
	//ethernet link must have gid
	if(parameters.Ethlink){
		if(ibv_query_gid(dev_ctx, 1, 0, &mygid)){
			fprintf(stderr,"failed to get gid for port 1 , index 0!\n");
		}
		memcpy(local_conn.gid, &mygid, sizeof(mygid));
	}


	MPI_Sendrecv(&local_conn, sizeof(struct ib_conn), MPI_CHAR, myrank ? 0 : 1, myrank ? 1 : 0,
	            &remote_conn, sizeof(struct ib_conn), MPI_CHAR, myrank ? 0 : 1, myrank ? 0 : 1,
		    MPI_COMM_WORLD, MPI_STATUS_IGNORE);
//	printf("rank%d_local : LID %#04x, QPN %#06x, PSN %#06x\nrank%d_remote: LID %#04x, QPN %#06x, PSN %#06x\n", myrank,  local_conn.lid,  local_conn.qpn,  local_conn.psn, myrank, remote_conn.lid, remote_conn.qpn, remote_conn.psn);
	if(parameters.Ethlink){
//		printf("\nRank%d  local_gid: %#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x;\
		\nRank%d remote_gid:%#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x:%#2x%#2x\n",\
		myrank, local_conn.gid[0], local_conn.gid[1], local_conn.gid[2],local_conn.gid[3],local_conn.gid[4],\
		local_conn.gid[5],local_conn.gid[6],local_conn.gid[7],local_conn.gid[8],local_conn.gid[9],\
		local_conn.gid[10],local_conn.gid[11],local_conn.gid[12],local_conn.gid[13],local_conn.gid[14],\
		local_conn.gid[15],myrank,\
		remote_conn.gid[0], remote_conn.gid[1], remote_conn.gid[2],remote_conn.gid[3],remote_conn.gid[4],\
		remote_conn.gid[5],remote_conn.gid[6],remote_conn.gid[7],remote_conn.gid[8],remote_conn.gid[9],\
		remote_conn.gid[10],remote_conn.gid[11],remote_conn.gid[12],remote_conn.gid[13],remote_conn.gid[14],\
		remote_conn.gid[15]);
	}
	
	// sender + receiver: set QP status to RTR
	qp_attr.qp_state              = IBV_QPS_RTR;
	qp_attr.path_mtu              = IBV_MTU_1024;
	qp_attr.dest_qp_num           = remote_conn.qpn;
	qp_attr.rq_psn                = remote_conn.psn;
	qp_attr.max_dest_rd_atomic    = 1;
	qp_attr.min_rnr_timer         = 0x12;
	qp_attr.ah_attr.is_global     = 0;
	qp_attr.ah_attr.dlid          = remote_conn.lid;
	qp_attr.ah_attr.sl            = 0;
	qp_attr.ah_attr.src_path_bits = 0;
	qp_attr.ah_attr.port_num      = IB_PORT;
	// ethernet link set gid parameters.
	if(parameters.Ethlink){
		qp_attr.ah_attr.is_global = 1;
		memcpy(&qp_attr.ah_attr.grh.dgid, &remote_conn.gid, 16);
		qp_attr.ah_attr.port_num = IB_PORT;
		qp_attr.ah_attr.grh.flow_label = 0;
		qp_attr.ah_attr.grh.sgid_index = 0;
		qp_attr.ah_attr.grh.hop_limit = 0xff;
		qp_attr.ah_attr.grh.traffic_class = 0;
	}

	if (ibv_modify_qp(qp, &qp_attr,
	                       IBV_QP_STATE              |
	                       IBV_QP_AV                 |
	                       IBV_QP_PATH_MTU           |
	                       IBV_QP_DEST_QPN           |
	                       IBV_QP_RQ_PSN             |
	                       IBV_QP_MAX_DEST_RD_ATOMIC |
						   IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "failed to modify qp state to RTR!\n");
		goto EXIT_DESTROY_QP;
	}

	// sender: set QP status to RTS; ready to perform data transfers
	if (0 == myrank) {
		qp_attr.qp_state      = IBV_QPS_RTS;
		qp_attr.timeout       = 14;
		qp_attr.retry_cnt     = 7;
		qp_attr.rnr_retry     = 7;
		qp_attr.sq_psn        = local_conn.psn;
		qp_attr.max_rd_atomic = 1;
		if (ibv_modify_qp(qp, &qp_attr,
		                       IBV_QP_STATE     |
		                       IBV_QP_TIMEOUT   |
		                       IBV_QP_RETRY_CNT |
		                       IBV_QP_RNR_RETRY |
		                       IBV_QP_SQ_PSN    |
		                       IBV_QP_MAX_QP_RD_ATOMIC)) {
			fprintf(stderr, "failed to modify qp state to RTS!\n");
			goto EXIT_DESTROY_QP;
		}
	}



	// bench S/G performance of send/recv
	for (int test = SGRS; test < TEST_END; test++) {
		struct ibv_send_wr *bad_wr;
		struct ibv_recv_wr *bad_rr;
		struct ibv_wc wc;

		unsigned char *buf;

		uint64_t tick,nictick;
		uint64_t copytotal,nictotal;
		int i, j, m, n;
		int ne;

		// prepare the buffers
		memset(buf_sg, 0x00, buf_size), memset(buf_cp, 0x00, buf_size);
		if (0 == myrank) {
			buf = (test == SGRS) ? (unsigned char *)buf_sg : (unsigned char *)buf_cp;
			for (i = 0; i < parameters.block_num; i++) {
				memset(buf + i * parameters.stride, 0xff, parameters.block_size);
			}
		}

		// prepare the S/G entries
		memset(sg_list, 0, sizeof(struct ibv_sge) * parameters.block_num);
		for (i = 0; i < parameters.block_num; i++) {
			sg_list[i].addr   = ((uintptr_t)buf_sg) + i * parameters.stride;
			sg_list[i].length = (test == SGRS) ? parameters.block_size : parameters.block_size * parameters.block_num;
			sg_list[i].lkey   = mr->lkey;

			if (test == SR_COPY) break;
		}

		if (myrank) {
			// receiver prepares the receive request
			rr = (const struct ibv_recv_wr){ 0 };
			rr.wr_id   = WR_ID;
			rr.sg_list = sg_list;
			rr.num_sge = (test == SGRS) ? parameters.block_num : 1;
		} else {
			// sender prepares the send request
			sr = (const struct ibv_send_wr){ 0 };
			sr.wr_id      = WR_ID;
			sr.sg_list    = sg_list;
			sr.num_sge    = (test == SGRS) ? parameters.block_num : 1;
			sr.opcode     = IBV_WR_SEND;
			sr.send_flags = IBV_SEND_SIGNALED;

			// then prepares timing
			min_tick = 0xffffffffffffffffUL;
			max_tick = 0;
			memset(ticks, 0, sizeof(uint64_t) * parameters.iterN);
			memset(copyticks, 0, sizeof(uint64_t) * parameters.iterN);
			memset(nicticks, 0, sizeof(uint64_t) * parameters.iterN);
		}

		// start iteration
		for (i = 0; i < (parameters.iterN + parameters.iterW); i++) {
			if (myrank) {
				// post receive WR
				if (ibv_post_recv(qp, &rr, &bad_rr)) {
					fprintf(stderr, "failed to post receive WR!\n");
					goto EXIT_DESTROY_QP;
				}

				// wait for send
				MPI_Barrier(MPI_COMM_WORLD);
			} else {
				// wait for recv
				MPI_Barrier(MPI_COMM_WORLD);

				// start timing at the sender side
				tick = rdtsc();

				// copy the buffer
				if (test == SR_COPY) {
					for (j = 0; j < parameters.block_num; j++) {
						memcpy((unsigned char *)buf_sg + j * parameters.block_size, (unsigned char *)buf_cp + j * parameters.stride, parameters.block_size);
					}
					if(i>=parameters.iterW)
						copyticks[i-parameters.iterW] = rdtsc() - tick; 
				}
				nictick = rdtsc();
				// post send WR
				if (ibv_post_send(qp, &sr, &bad_wr)) {
					fprintf(stderr, "failed to post WR!\n");
					goto EXIT_DESTROY_QP;
				}
			}

			// poll the CQ
			do ne = ibv_poll_cq(cq, 1, &wc); while (ne == 0);
			if (ne < 0) {
				fprintf(stderr, "rank%d failed to read CQ!\n", myrank);
				goto EXIT_DESTROY_QP;
			}
			if (wc.status != IBV_WC_SUCCESS) {
				fprintf(stderr, "rank%d failed to execute WR!\n", myrank);
				goto EXIT_DESTROY_QP;
			}
			if(i>=parameters.iterW)
				nicticks[i-parameters.iterW] = rdtsc() - nictick; 
		
			if (myrank) {
				// receiver verifies the buffer
				buf = (unsigned char *)buf_sg;
				if (test == SGRS) {
					for (m = 0; m < parameters.block_num;  m++) {
					for (n = 0; n < parameters.block_size; n++) {
						j = m * parameters.stride + n;
						if (buf[j] != 0xff || (buf[j] = 0x00)) {
							fprintf(stderr, "failed to verify the received data @%d!\n", j);
							break;
						}
					}}
				} else {
					for (j = 0; j < parameters.block_size * parameters.block_num; j++) {
						if (buf[j] != 0xff || (buf[j] = 0x00)) {
							fprintf(stderr, "failed to verify the received data @%d!\n", j);
							break;
						}
					}
				}
			} else {
				// copy the buffer
				if (test == SR_COPY) {
					for (j = 0; j < parameters.block_num; j++) {
						memcpy((unsigned char *)buf_cp + j * parameters.stride, (unsigned char *)buf_sg + j * parameters.block_size, parameters.block_size);
					}
				}

				// finish timing at sender side
				tick = rdtsc() - tick;
				if (i >= parameters.iterW) {
					ticks[i-parameters.iterW] = tick;
					if (tick < min_tick) min_tick = tick;
					if (tick > max_tick) max_tick = tick;
				}
			}
		}//end of iterations
		
		// print timing result
		if (0 == myrank) {
			printf("[%s] ", (test == SGRS) ? "sgrs" : "sr_copy");
			copytotal = 0;
			nictotal = 0;
			for(j = 0; j<parameters.iterN; j++){
				copytotal += copyticks[j];
				nictotal += nicticks[j];
			}
			printf("oneside avg copy overhead: %.3f us, nic comm avg overhead: %.3f\nblock_size = %d, block_num = %d\n", (float)copytotal/parameters.iterN/TICKS_PER_USEC, (float)nictotal/parameters.iterN/TICKS_PER_USEC, parameters.block_size, parameters.block_num);
			print_timing(parameters.iterN);
		}
	}//end of situation

EXIT_DESTROY_QP:
	ibv_destroy_qp(qp);

EXIT_DESTROY_CQ:
	ibv_destroy_cq(cq);

EXIT_DEREG_MR:
	ibv_dereg_mr(mr);

EXIT_FREE_BUF:
	if (buf_sg) free(buf_sg);
	if (buf_cp) free(buf_cp);

EXIT_DEALLOC_PD:
	ibv_dealloc_pd(pd);

EXIT_CLOSE_DEV:
	ibv_close_device(dev_ctx);

EXIT_FREE_DEV_LIST:
	ibv_free_device_list(dev_list);

EXIT_MPI_FINALIZE:
	MPI_Finalize();

	return 0;
}

