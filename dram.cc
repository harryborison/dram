// Copyright (c) 2009-2011, Tor M. Aamodt, Wilson W.L. Fung, Ali Bakhoda,
// Ivan Sham, George L. Yuan,
// The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice, this
// list of conditions and the following disclaimer in the documentation and/or
// other materials provided with the distribution.
// Neither the name of The University of British Columbia nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <string>
#include "gpu-sim.h"
#include "gpu-misc.h"
#include "dram.h"
#include "mem_latency_stat.h"
#include "dram_sched.h"
#include "mem_fetch.h"
#include "l2cache.h"
#include <zlib.h>
#include <fcntl.h>
#include <sstream>
#define DRAM_VERIFY
#ifdef DRAM_VERIFY
int PRINT_CYCLE=0;
#endif
#define BUF_MAX 1024
int wcnt=0;
int total_cnt=0;
int first = 0;
int Befor_cycle=0;
int Sum_cycle=0;
long long int buf[1024];
int bufcnt=0;
int Veri=0;
int fd; 
FILE *fp;
char * gzfilename = "trace.gz";
template class fifo_pipeline<mem_fetch>;
template class fifo_pipeline<dram_req_t>;

dram_t::dram_t( unsigned int partition_id, const struct memory_config *config, memory_stats_t *stats,
                memory_partition_unit *mp )
{
   id = partition_id;
   m_memory_partition_unit = mp;
   m_stats = stats;
   m_config = config;

   //rowblp
   access_num=0;
   hits_num=0;
   read_num=0;
   write_num=0;
   hits_read_num=0;
   hits_write_num=0;
   banks_1time=0;
   banks_acess_total=0;
   banks_acess_total_after=0;
   banks_time_ready=0;
   banks_access_ready_total=0;
   issued_two=0;
   issued_total=0;
   issued_total_row=0;
   issued_total_col=0;

   CCDc = 0;
   RRDc = 0;
   RTWc = 0;
   WTRc = 0;

   wasted_bw_row=0;
   wasted_bw_col=0;
   util_bw=0;
   idle_bw=0;
   RCDc_limit=0;
   CCDLc_limit=0;
   CCDLc_limit_alone=0;
   CCDc_limit=0;
   WTRc_limit=0;
   WTRc_limit_alone=0;
   RCDWRc_limit=0;
   RTWc_limit=0;
   RTWc_limit_alone=0;
   rwq_limit=0;
   write_to_read_ratio_blp_rw_average=0;
   bkgrp_parallsim_rw=0;

   rw = READ; //read mode is default

	bkgrp = (bankgrp_t**) calloc(sizeof(bankgrp_t*), m_config->nbkgrp);
	bkgrp[0] = (bankgrp_t*) calloc(sizeof(bank_t), m_config->nbkgrp);
	for (unsigned i=1; i<m_config->nbkgrp; i++) {
		bkgrp[i] = bkgrp[0] + i;
	}
	for (unsigned i=0; i<m_config->nbkgrp; i++) {
		bkgrp[i]->CCDLc = 0;
		bkgrp[i]->RTPLc = 0;
	}

   bk = (bank_t**) calloc(sizeof(bank_t*),m_config->nbk);
   bk[0] = (bank_t*) calloc(sizeof(bank_t),m_config->nbk);
   for (unsigned i=1;i<m_config->nbk;i++) 
      bk[i] = bk[0] + i;
   for (unsigned i=0;i<m_config->nbk;i++) {
      bk[i]->state = BANK_IDLE;
      bk[i]->bkgrpindex = i/(m_config->nbk/m_config->nbkgrp);
      bk[i]->REFi = m_config->tREFi;
      bk[i]->refresh_pending = 0;
   }
   prio = 0;

   rwq = new fifo_pipeline<dram_req_t>("rwq",m_config->CL,m_config->CL+1);
   mrqq = new fifo_pipeline<dram_req_t>("mrqq",0,2);
   returnq = new fifo_pipeline<mem_fetch>("dramreturnq",0,m_config->gpgpu_dram_return_queue_size==0?1024:m_config->gpgpu_dram_return_queue_size); 
   m_frfcfs_scheduler = NULL;
   if ( m_config->scheduler_type == DRAM_FRFCFS)
      m_frfcfs_scheduler = new frfcfs_scheduler(m_config,this,stats);
   n_cmd = 0;
   n_activity = 0;
   n_nop = 0; 
   n_act = 0; 
   n_pre = 0; 
   n_rd = 0;
   n_wr = 0;
   n_wr_WB=0;
   n_rd_L2_A=0;
   n_req = 0;
   max_mrqs_temp = 0;
   bwutil = 0;
   max_mrqs = 0;
   ave_mrqs = 0;

   for (unsigned i=0;i<10;i++) {
      dram_util_bins[i]=0;
      dram_eff_bins[i]=0;
   }
   last_n_cmd = last_n_activity = last_bwutil = 0;

   n_cmd_partial = 0;
   n_activity_partial = 0;
   n_nop_partial = 0;  
   n_act_partial = 0;  
   n_pre_partial = 0;  
   n_req_partial = 0;
   ave_mrqs_partial = 0;
   bwutil_partial = 0;

   if ( queue_limit() )
      mrqq_Dist = StatCreate("mrqq_length",1, queue_limit());
   else //queue length is unlimited; 
      mrqq_Dist = StatCreate("mrqq_length",1,64); //track up to 64 entries

}

bool dram_t::full(bool is_write) const
{
    if(m_config->scheduler_type == DRAM_FRFCFS){
        if(m_config->gpgpu_frfcfs_dram_sched_queue_size == 0 ) return false;
        if(m_config->seperate_write_queue_enabled){
        	if(is_write)
        		return m_frfcfs_scheduler->num_write_pending() >= m_config->gpgpu_frfcfs_dram_write_queue_size;
        	else
        		return m_frfcfs_scheduler->num_pending() >= m_config->gpgpu_frfcfs_dram_sched_queue_size;
        }
        else
        	return m_frfcfs_scheduler->num_pending() >= m_config->gpgpu_frfcfs_dram_sched_queue_size;
    }
   else return mrqq->full();
}

unsigned dram_t::que_length() const
{
   unsigned nreqs = 0;
   if (m_config->scheduler_type == DRAM_FRFCFS) {
      nreqs = m_frfcfs_scheduler->num_pending();
   } else {
      nreqs = mrqq->get_length();
   }
   return nreqs;
}

bool dram_t::returnq_full() const
{
   return returnq->full();
}

unsigned int dram_t::queue_limit() const 
{ 
   return m_config->gpgpu_frfcfs_dram_sched_queue_size; 
}


dram_req_t::dram_req_t( class mem_fetch *mf, unsigned banks, unsigned dram_bnk_indexing_policy)
{
   txbytes = 0;
   dqbytes = 0;
   data = mf;

   const addrdec_t &tlx = mf->get_tlx_addr();

    switch(dram_bnk_indexing_policy){
		case LINEAR_BK_INDEX:
		{
			bk = tlx.bk;
			break;
		}
		case BITWISE_XORING_BK_INDEX:
		{
			//xoring bank bits with lower bits of the page
			int lbank = log2(banks);
			bk  = tlx.bk ^ (tlx.row & ((1<<lbank)-1));
			break;
		}
		case CUSTOM_BK_INDEX:
	        /* No custom set function implemented */
			//Do you custom index here
			break;
		default:
			 assert("\nUndefined bank index function.\n" && 0);
			 break;
	}


   row = tlx.row; 
   col = tlx.col; 
   nbytes = mf->get_data_size();

   timestamp = gpu_tot_sim_cycle + gpu_sim_cycle;
  
   addr = mf->get_addr();
   insertion_time = (unsigned) gpu_sim_cycle;
   rw = data->get_is_write()?WRITE:READ;
}

void dram_t::push( class mem_fetch *data ) 
{
   int tot_len = 0;
   std::stringstream s;
   std::string temp;
   char mmsg[2000]= {0,}; 
  // std::string msg;
   char msg[1024]={0,};
   if(wcnt==0)
   {
   fp=fopen("trace","w");
    

   wcnt=1;
   }

   assert(id == data->get_tlx_addr().chip); // Ensure request is in correct memory partition
   dram_req_t *mrq = new dram_req_t(data,m_config->nbk,m_config->dram_bnk_indexing_policy);

   data->set_status(IN_PARTITION_MC_INTERFACE_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);
	   mrqq->push(mrq);
//   printf("in push %lld  %c \n",mrq->addr,mrq->rw);
//  fputs(mrq->addr,fp);
//   fprintf(fp,"%llx %c %lld",mrq->addr,mrq->rw,gpu_sim_cycle);
   fprintf(fp,"%lld %llx %c\n",mrq->timestamp,mrq->addr,mrq->rw);
//   const addrdec_t &tmp2 = data->get_tlx_addr();
//   tmp2.print(fp);
//   fprintf(fp,"\n");
   //fprintf(fp,"%lld %llx %c\n\n",mrq->timestamp,data->get_tlx_addr(),mrq->rw);
//   fprintf(stdout,"%llx %c %lld",mrq->addr,mrq->rw,gpu_sim_cycle);
   
   

   s<<mrq->addr <<' '<< mrq->rw << ' ' << gpu_sim_cycle <<'\n';
   temp = s.str().c_str();
   


   n_req += 1;
   n_req_partial += 1;
   if ( m_config->scheduler_type == DRAM_FRFCFS) {
      unsigned nreqs = m_frfcfs_scheduler->num_pending();
      if ( nreqs > max_mrqs_temp)
         max_mrqs_temp = nreqs;
   } else {
      max_mrqs_temp = (max_mrqs_temp > mrqq->get_length())? max_mrqs_temp : mrqq->get_length();
   }
   m_stats->memlatstat_dram_access(data);
}

void dram_t::scheduler_fifo()
{
   if (!mrqq->empty()) {
      unsigned int bkn;
      dram_req_t *head_mrqq = mrqq->top();
      head_mrqq->data->set_status(IN_PARTITION_MC_BANK_ARB_QUEUE,gpu_sim_cycle+gpu_tot_sim_cycle);
      bkn = head_mrqq->bk;
      if (!bk[bkn]->mrq) 
         bk[bkn]->mrq = mrqq->pop();
   }
}


#define DEC2ZERO(x) x = (x)? (x-1) : 0;
#define SWAP(a,b) a ^= b; b ^= a; a ^= b;

void dram_t::cycle()
{
   if( !returnq->full() ) {
       dram_req_t *cmd = rwq->pop();
       if( cmd ) {
#ifdef DRAM_VIEWCMD 
           printf("\tDQ: BK%d Row:%03x Col:%03x", cmd->bk, cmd->row, cmd->col + cmd->dqbytes);
#endif
           cmd->dqbytes += m_config->dram_atom_size; 

           if (cmd->dqbytes >= cmd->nbytes) {
              mem_fetch *data = cmd->data; 
              data->set_status(IN_PARTITION_MC_RETURNQ,gpu_sim_cycle+gpu_tot_sim_cycle); 
              if( data->get_access_type() != L1_WRBK_ACC && data->get_access_type() != L2_WRBK_ACC ) {
		   data->set_reply();
               
		  returnq->push(data);
              } else {
                 m_memory_partition_unit->set_done(data);
                 delete data;
              }
              delete cmd;
           }
#ifdef DRAM_VIEWCMD 
           printf("\n");
#endif
       }
   }

   /* check if the upcoming request is on an idle bank */
   /* Should we modify this so that multiple requests are checked? */

   switch (m_config->scheduler_type) {
   case DRAM_FIFO: scheduler_fifo(); break;
   case DRAM_FRFCFS: scheduler_frfcfs(); break;
	default:
		printf("Error: Unknown DRAM scheduler type\n");
		assert(0);
   }
   if ( m_config->scheduler_type == DRAM_FRFCFS) {
      unsigned nreqs = m_frfcfs_scheduler->num_pending();
      if ( nreqs > max_mrqs) {
         max_mrqs = nreqs;
      }
      ave_mrqs += nreqs;
      ave_mrqs_partial += nreqs;
   } else {
      if (mrqq->get_length() > max_mrqs) {
         max_mrqs = mrqq->get_length();
      }
      ave_mrqs += mrqq->get_length();
      ave_mrqs_partial +=  mrqq->get_length();
   }

   unsigned k=m_config->nbk;
   bool issued = false;

   //collect row buffer locality, BLP and other statistics
   /////////////////////////////////////////////////////////////////////////
   unsigned int memory_pending=0;
   for (unsigned i=0;i<m_config->nbk;i++) {
	   if (bk[i]->mrq)
		   memory_pending++;
   }
   banks_1time += memory_pending;
   if(memory_pending >0)
	   banks_acess_total++;

   unsigned int memory_pending_rw=0;
   unsigned read_blp_rw=0;
   unsigned write_blp_rw=0;
   std::bitset<8> bnkgrp_rw_found;  //assume max we have 8 bank groups

   for (unsigned j=0;j<m_config->nbk;j++) {
  	   unsigned grp = get_bankgrp_number(j);
  	   if (bk[j]->mrq && (((bk[j]->curr_row == bk[j]->mrq->row) &&
  		  (bk[j]->mrq->rw == READ)  &&
  		  (bk[j]->state == BANK_ACTIVE))))
  	   {
  		    memory_pending_rw++;
  		    read_blp_rw++;
  		    bnkgrp_rw_found.set(grp);
  	   }
  	   else if
  	        (bk[j]->mrq && (((bk[j]->curr_row == bk[j]->mrq->row)  &&
  			 (bk[j]->mrq->rw == WRITE) &&
  			 (bk[j]->state == BANK_ACTIVE))))
  	   {
  		     memory_pending_rw++;
  		     write_blp_rw++;
  		     bnkgrp_rw_found.set(grp);
  	   }
     }
     banks_time_rw += memory_pending_rw;
     bkgrp_parallsim_rw += bnkgrp_rw_found.count();
     if(memory_pending_rw >0)
     {
    	 write_to_read_ratio_blp_rw_average += (double)write_blp_rw/(write_blp_rw+read_blp_rw);
  	     banks_access_rw_total++;
     }

   unsigned int memory_Pending_ready=0;
   for (unsigned j=0;j<m_config->nbk;j++) {
	   unsigned grp = get_bankgrp_number(j);
	   if (bk[j]->mrq && ((!CCDc && !bk[j]->RCDc &&
		  !(bkgrp[grp]->CCDLc) &&
		  (bk[j]->curr_row == bk[j]->mrq->row) &&
		  (bk[j]->mrq->rw == READ) && (WTRc == 0 )  &&
		  (bk[j]->state == BANK_ACTIVE) &&
		  !rwq->full())
		   ||
		   (!CCDc && !bk[j]->RCDWRc &&
			 !(bkgrp[grp]->CCDLc) &&
			 (bk[j]->curr_row == bk[j]->mrq->row)  &&
			 (bk[j]->mrq->rw == WRITE) && (RTWc == 0 )  &&
			 (bk[j]->state == BANK_ACTIVE) &&
			 !rwq->full())))
	   {
		   memory_Pending_ready++;
	   }
   }
   banks_time_ready += memory_Pending_ready;
   if(memory_Pending_ready >0)
	   banks_access_ready_total++;

    for (unsigned j=0;j<m_config->nbk;j++)
    {	
        if(!bk[j]->mrq && bk[j]->refresh_pending > 0)
        {
		if(bk[j]->refresh_pending > m_config->tREFi)
		{
			bk[j] ->RFC = m_config->tRFC + (bk[j]->refresh_pending/m_config->tREFi)*m_config->tRFC;
		}
		else
		{
                bk[j]->RFC = m_config->tRFC;
		}
		printf("\nRFC Insert %d \n",bk[j]->RFC);
                bk[j]->refresh_pending = 0;
        }
	else if(bk[j]->mrq && bk[j]->refresh_pending >0 ) // need refresh but not able to start
	{
		bk[j]->refresh_pending++;

	}


    }

   ///////////////////////////////////////////////////////////////////////////////////

   bool issued_col_cmd = false;
   bool issued_row_cmd = false;
   bool issued_refresh_cmd = false;
   if(m_config->dual_bus_interface)
   {
	   //dual bus interface
	   //issue one row command and one column command
	     for (unsigned i=0;i<m_config->nbk;i++) {

	       unsigned j = (i + prio) % m_config->nbk;
		if(bk[j]->RFC>0 && bk[j]->state!= BANK_REFRESH)
		{
	           issued_refresh_cmd = issue_refresh_command(j);
		}
		else if(bk[j]->state==BANK_REFRESH && bk[j]->RFC==0)
		{
		//refresh end
			bk[j]->state =BANK_IDLE;
		}	
		   
	     }
	     for (unsigned i=0;i<m_config->nbk;i++) {
	       unsigned j = (i + prio) % m_config->nbk;
		   if(bk[j]->RFC>0) break; 

	  	   issued_col_cmd  = issue_col_command(j);
	  	   if(issued_col_cmd) break;
	     }
	     for (unsigned i=0;i<m_config->nbk;i++) {
	    	unsigned j = (i + prio) % m_config->nbk;
		    if(bk[j]->RFC >0) break; // refresh

	  	    issued_row_cmd = issue_row_command(j);
	  	    if(issued_row_cmd) break;
	     }
		
	     for (unsigned i=0;i<m_config->nbk;i++) {
	    	 unsigned j = (i + prio) % m_config->nbk;
	    	 if(!bk[j]->mrq) {
				  if (!CCDc && !RRDc && !RTWc && !WTRc && !bk[j]->RCDc && !bk[j]->RASc
					  && !bk[j]->RCc && !bk[j]->RPc  && !bk[j]->RCDWRc) k--;
				  bk[j]->n_idle++;
			}
	     }
   }
   else
   {
	   //single bus interface
	   //issue only one row/column command
	   for (unsigned i=0;i<m_config->nbk;i++) {
		 unsigned j = (i + prio) % m_config->nbk;
	       	 if(bk[j]->RFC>0 && bk[j]->state!= BANK_REFRESH)
                 {
                      issued_refresh_cmd = issue_refresh_command(j);
                 }
                 else if(bk[j]->state==BANK_REFRESH && bk[j]->RFC==0)
                 {
                //refresh end
                      bk[j]->state =BANK_IDLE;
		      //printf("\nRefresh End , BK[%d] is IDLE \n",j);
                 }
		 if(bk[j]->RFC== 0 )
		 {
         	   if(!issued_col_cmd)
		       issued_col_cmd  = issue_col_command(j);

		   if(!issued_col_cmd && !issued_row_cmd)
		       issued_row_cmd = issue_row_command(j);
 		 }
		   if(!bk[j]->mrq) {
		          if (!CCDc && !RRDc && !RTWc && !WTRc && !bk[j]->RCDc && !bk[j]->RASc
		              && !bk[j]->RCc && !bk[j]->RPc  && !bk[j]->RCDWRc) k--;
		          bk[j]->n_idle++;
		    }

	   }
   }

   issued = issued_row_cmd || issued_col_cmd;
   if (!issued) {
      n_nop++;
      n_nop_partial++;
#ifdef DRAM_VIEWCMD
      printf("\tNOP                        ");
#endif
   }
   if (k) {
      n_activity++;
      n_activity_partial++;
   }
   n_cmd++;
   n_cmd_partial++;
   if(issued)
   {
	   issued_total++;
	   if(issued_col_cmd && issued_row_cmd)
		   issued_two++;
   }
   if(issued_col_cmd)  issued_total_col++;
   if(issued_row_cmd)  issued_total_row++;


   //Collect some statistics
   //check the limitation, see where BW is wasted?
   /////////////////////////////////////////////////////////
   unsigned int memory_pending_found=0;
      for (unsigned i=0;i<m_config->nbk;i++) {
   	   if (bk[i]->mrq)
   		memory_pending_found++;
      }
      if(memory_pending_found>0)
    	  banks_acess_total_after++;

        bool memory_pending_rw_found=false;
        for (unsigned j=0;j<m_config->nbk;j++) {
     	   unsigned grp = get_bankgrp_number(j);
     	   if (bk[j]->mrq && (((bk[j]->curr_row == bk[j]->mrq->row) &&
     		  (bk[j]->mrq->rw == READ)  &&
     		  (bk[j]->state == BANK_ACTIVE))
     		   ||
     		   (
     			 (bk[j]->curr_row == bk[j]->mrq->row)  &&
     			 (bk[j]->mrq->rw == WRITE) &&
     			 (bk[j]->state == BANK_ACTIVE))))
     		  memory_pending_rw_found=true;
        }


   if(issued_col_cmd  || CCDc)
	   util_bw++;
   else if (memory_pending_rw_found)
   {
	   wasted_bw_col++;
	   for (unsigned j=0;j<m_config->nbk;j++) {
		   unsigned grp = get_bankgrp_number(j);
		   //read
		   if (bk[j]->mrq && (((bk[j]->curr_row == bk[j]->mrq->row) &&
			  (bk[j]->mrq->rw == READ)  &&
			  (bk[j]->state == BANK_ACTIVE))))
		   {
			   if(bk[j]->RCDc) RCDc_limit++;
			   if(bkgrp[grp]->CCDLc) CCDLc_limit++;
			   if(WTRc) WTRc_limit++;
			   if(CCDc) CCDc_limit++;
			   if(rwq->full()) rwq_limit++;
			   if(bkgrp[grp]->CCDLc && !WTRc) CCDLc_limit_alone++;
			   if(!bkgrp[grp]->CCDLc && WTRc) WTRc_limit_alone++;
		   }
		   //write
		   else if (bk[j]->mrq && ((bk[j]->curr_row == bk[j]->mrq->row)  &&
				 (bk[j]->mrq->rw == WRITE) &&
				 (bk[j]->state == BANK_ACTIVE)))
		   {
			   if(bk[j]->RCDWRc) RCDWRc_limit++;
			   if(bkgrp[grp]->CCDLc) CCDLc_limit++;
			   if(RTWc) RTWc_limit++;
			   if(CCDc) CCDc_limit++;
			   if(rwq->full()) rwq_limit++;
			   if(bkgrp[grp]->CCDLc && !RTWc) CCDLc_limit_alone++;
			   if(!bkgrp[grp]->CCDLc && RTWc) RTWc_limit_alone++;
		   }
	     }
   }
   else if (memory_pending_found)
	   wasted_bw_row++;
   else if (!memory_pending_found)
  	   idle_bw++;
   else
	   assert(1);

   /////////////////////////////////////////////////////////

   // decrements counters once for each time dram_issueCMD is called


   DEC2ZERO(RRDc);
   DEC2ZERO(CCDc);
   DEC2ZERO(RTWc);
   DEC2ZERO(WTRc);
   for (unsigned j=0;j<m_config->nbk;j++) {
      DEC2ZERO(bk[j]->RCDc);
      DEC2ZERO(bk[j]->RASc);
      DEC2ZERO(bk[j]->RCc);
      DEC2ZERO(bk[j]->RPc);
      DEC2ZERO(bk[j]->RCDWRc);
      DEC2ZERO(bk[j]->WTPc);
      DEC2ZERO(bk[j]->RTPc);
      DEC2ZERO(bk[j]->REFi);

      if(bk[j]->state == BANK_REFRESH) 
	{
	  DEC2ZERO(bk[j]->RFC);
	}
   }
   for (unsigned j=0; j<m_config->nbkgrp; j++) {
	   DEC2ZERO(bkgrp[j]->CCDLc);
	   DEC2ZERO(bkgrp[j]->RTPLc);
   }
   for (unsigned j=0; j<m_config->nbk;j++) {
	if(bk[j]->REFi == 0 ) // start refresh
	{
		printf("\n %lld bk[%d] Refresh needed \n",gpu_tot_sim_cycle+gpu_sim_cycle,j);
		bk[j]->REFi = m_config->tREFi;
		bk[j]->refresh_pending ++;
	}
	
//	if(!bk[j]->mrq && bk[j]->refresh_pending > 0)
//	{
//		bk[j]->RFC = m_config->tRFC;
//		bk[j]->refresh_pending = 0;
//	}


   }

#ifdef DRAM_VISUALIZE
   visualize();
#endif
}

bool dram_t::issue_col_command(int j)
{
	bool issued = false;
	unsigned grp = get_bankgrp_number(j);
    if (bk[j]->mrq) { //if currently servicing a memory request
        bk[j]->mrq->data->set_status(IN_PARTITION_DRAM,gpu_sim_cycle+gpu_tot_sim_cycle);
       // correct row activated for a READ
       if ( !issued && !CCDc && !bk[j]->RCDc &&
            !(bkgrp[grp]->CCDLc) &&
            (bk[j]->curr_row == bk[j]->mrq->row) &&
            (bk[j]->mrq->rw == READ) && (WTRc == 0 )  &&
            (bk[j]->state == BANK_ACTIVE) &&
            !rwq->full() ) {
          if (rw==WRITE) {
             rw=READ;
             rwq->set_min_length(m_config->CL);
          }
          rwq->push(bk[j]->mrq);
          bk[j]->mrq->txbytes += m_config->dram_atom_size;
          CCDc = m_config->tCCD;
          bkgrp[grp]->CCDLc = m_config->tCCDL;
          RTWc = m_config->tRTW;
          bk[j]->RTPc = m_config->BL/m_config->data_command_freq_ratio;
          bkgrp[grp]->RTPLc = m_config->tRTPL;
          issued = true;
          if(bk[j]->mrq->data->get_access_type() == L2_WR_ALLOC_R)
        	  n_rd_L2_A++;
          else
                n_rd++;

          bwutil += m_config->BL/m_config->data_command_freq_ratio;
          bwutil_partial += m_config->BL/m_config->data_command_freq_ratio;
          bk[j]->n_access++;

#ifdef DRAM_VERIFY
          PRINT_CYCLE=1;
          printf("\t%lld RD  Bk:%d Row:%03x Col:%03x \n",gpu_tot_sim_cycle+gpu_sim_cycle,
                 j, bk[j]->curr_row,
                 bk[j]->mrq->col + bk[j]->mrq->txbytes - m_config->dram_atom_size);
#endif
          // transfer done
          if ( !(bk[j]->mrq->txbytes < bk[j]->mrq->nbytes) ) {
             bk[j]->mrq = NULL;
          }
       } else
          // correct row activated for a WRITE
          if ( !issued && !CCDc && !bk[j]->RCDWRc &&
               !(bkgrp[grp]->CCDLc) &&
               (bk[j]->curr_row == bk[j]->mrq->row)  &&
               (bk[j]->mrq->rw == WRITE) && (RTWc == 0 )  &&
               (bk[j]->state == BANK_ACTIVE) &&
               !rwq->full() ) {
          if (rw==READ) {
             rw=WRITE;
             rwq->set_min_length(m_config->WL);
          }
          rwq->push(bk[j]->mrq);

          bk[j]->mrq->txbytes += m_config->dram_atom_size;
          CCDc = m_config->tCCD;
          bkgrp[grp]->CCDLc = m_config->tCCDL;
          WTRc = m_config->tWTR;
          bk[j]->WTPc = m_config->tWTP;
          issued = true;

          if(bk[j]->mrq->data->get_access_type() == L2_WRBK_ACC)
          	n_wr_WB++;
          else
          	 n_wr++;
          bwutil += m_config->BL/m_config->data_command_freq_ratio;
          bwutil_partial += m_config->BL/m_config->data_command_freq_ratio;
#ifdef DRAM_VERIFY
          PRINT_CYCLE=1;
          printf("\t%lld WR  Bk:%d Row:%03x Col:%03x \n",gpu_sim_cycle+ gpu_tot_sim_cycle
                 ,j, bk[j]->curr_row,
                 bk[j]->mrq->col + bk[j]->mrq->txbytes - m_config->dram_atom_size);
#endif
          // transfer done
          if ( !(bk[j]->mrq->txbytes < bk[j]->mrq->nbytes) ) {
             bk[j]->mrq = NULL;
          }
       }

    }

    return issued;
}
bool dram_t::issue_refresh_command(int j)
{
        unsigned grp = get_bankgrp_number(j);
	bool issued = false;
	if(bk[j]->state == BANK_ACTIVE)
	{
		if( (!issued) &&
               (bk[j]->state == BANK_ACTIVE) &&
               (!bk[j]->RASc && !bk[j]->WTPc &&
                                  !bk[j]->RTPc &&
                                  !bkgrp[grp]->RTPLc) )
		{
		//printf("\nBK[%d] is ready to refresh state is  IDLE(Precharge)\n",j);
		bk[j]->state = BANK_IDLE;
		}
		else 
			return false;


	}
	else if(bk[j]->state == BANK_IDLE)
	{
		 if ( !RRDc &&
               		(bk[j]->state == BANK_IDLE) &&
               		!bk[j]->RPc && !bk[j]->RCc)
		{
			printf("\n%lld BK[%d] start Refresh(State changed to Refresh)\n",gpu_sim_cycle+gpu_tot_sim_cycle,j);
			bk[j]->state = BANK_REFRESH;
		}
		else
			return false;


	}
	return issued;
}
bool dram_t::issue_row_command(int j)
{
	bool issued = false;
	unsigned grp = get_bankgrp_number(j);
    if (bk[j]->mrq) { //if currently servicing a memory request
        bk[j]->mrq->data->set_status(IN_PARTITION_DRAM,gpu_sim_cycle+gpu_tot_sim_cycle);
      //     bank is idle
    //else
//	  printf("bk[%d]->cur_row : %ld\n",j,bk[j]->curr_row);
	 
	 bk[j]->rowcount[bk[j]->curr_row]++;
  	  if ( !issued && !RRDc &&
               (bk[j]->state == BANK_IDLE) &&
               !bk[j]->RPc && !bk[j]->RCc) { // refresh condition 2
#ifdef DRAM_VERIFY
          PRINT_CYCLE=1;
          printf("\t%lld ACT BK:%d NewRow:%03x From:%03x \n",gpu_sim_cycle+gpu_tot_sim_cycle
                 ,j,bk[j]->mrq->row,bk[j]->curr_row);
#endif
          // activate the row with current memory request
	total_cnt++;
//        printf("bk[%d]->cur_row issue in IDLE %ld \n",j,bk[j]->curr_row);
	  bk[j]->curr_row = bk[j]->mrq->row;
    //     bk[j]->rowcount[bk[j]->curr_row]++;
	Sum_cycle=Sum_cycle +  gpu_sim_cycle - Befor_cycle;
	Befor_cycle = gpu_sim_cycle;
	if(first==0)
	{
	first=1;
	Sum_cycle = 0;

	}
//	printf("gpgpu_cycle : %lld total : %lld \n",gpu_sim_cycle,gpu_tot_sim_cycle);
	 bk[j]->state = BANK_ACTIVE;
          RRDc = m_config->tRRD;
	  //bk[j]->RFC = m_config->tRFC;
	  //bk[j]->REFi = m_config->tREFi;
          bk[j]->RCDc = m_config->tRCD;
          bk[j]->RCDWRc = m_config->tRCDWR;
          bk[j]->RASc = m_config->tRAS;
          bk[j]->RCc = m_config->tRC;
	  //printf("BANK : %d , RFC : %d , REFi : %d \n",j,bk[j]->RFC,bk[j]->REFi);
          prio = (j + 1) % m_config->nbk;
          issued = true;
          n_act_partial++;
          n_act++;
       }

       else
          // different row activated
          if ( (!issued) &&
               (bk[j]->curr_row != bk[j]->mrq->row) &&
               (bk[j]->state == BANK_ACTIVE) &&
               (!bk[j]->RASc && !bk[j]->WTPc &&
				  !bk[j]->RTPc &&
				  !bkgrp[grp]->RTPLc) ) { //refresh condition 1 ;
	total_cnt++; 
//	printf("bk[%d]->curr_row issue in ACTIVE %d \n",j,bk[j]->curr_row);
//	printf("gpgpu_cycle : %lld total : %lld\n",gpu_sim_cycle,gpu_tot_sim_cycle);

	Sum_cycle= Sum_cycle + gpu_sim_cycle - Befor_cycle;
	Befor_cycle = gpu_sim_cycle;
	if(first == 0)
	{
	first=1;
	Sum_cycle = 0;
	}	
	  bk[j]->rowcount[bk[j]->curr_row]++;
          // make the bank idle again
	  //printf("\nBANK_IDLE in Refresh _row_command\n");
          bk[j]->state = BANK_IDLE;
          bk[j]->RPc = m_config->tRP;
          prio = (j + 1) % m_config->nbk;
          issued = true;
          n_pre++;
          n_pre_partial++;
#ifdef DRAM_VERIFY
          PRINT_CYCLE=1;
          printf("\t%lld PRE BK:%d Row:%03x \n",gpu_tot_sim_cycle+gpu_sim_cycle, j,bk[j]->curr_row);
#endif
       }
    }
    return issued;
}


//if mrq is being serviced by dram, gets popped after CL latency fulfilled
class mem_fetch* dram_t::return_queue_pop()
{
    return returnq->pop();
}

class mem_fetch* dram_t::return_queue_top()
{
    return returnq->top();
}









void compress(char *input)

{
    char destFile[20], buf[BUF_MAX];

    gzFile pDestFile;

    FILE* pOriFile;

    int curRead = 0;

    

    // 압축파일의 이름은 filename.gz 으로 한다.

    //gzfilename = (char *)malloc(strlen(filename)*sizeof(char));

    sprintf(destFile, "%s.gz", input);

 

 

    // 원본 파일 오픈

    if ((pOriFile = fopen(input, "rb")) < 0)
    {
        printf("original file open error\n");
        exit(0);    
    }

    if ((pDestFile = gzopen(destFile, "wb")) == NULL)

    {

        printf("dest file open error\n");

        exit(0);

    }

 

    // 원본파일을 에서 데이타를 읽어들이고

    // gzwrite함수를 이용해서 데이터를 압축하고 파일에 쓴다.   
/*
    while((curRead = fread(buf, sizeof(char), BUF_MAX, pOriFile)) > 0)

    {

        if (gzwrite(pDestFile, buf, curRead) < 0)

        {

            //printf("%s\n",gzerror(zfp, &lerrno));

            printf("error in the while loop!! \n");

            exit(0);

        }

    }
*/
 



 


 

}






void uncompress(char *oriFile)

{

    char destFile[30], buf[BUF_MAX];

    gzFile pOriFile;

    FILE* pDestFile;

    int curRead=0, totalRead=0;

 

       


    int oriNameLength = strlen(oriFile) - 3;

 

//    strncpy(destFile, oriFile, oriNameLength);
//
  //  destFile[oriNameLength] = '\0';
	strcpy(destFile,"Ori.txt");
 

 

    // 압축이 풀릴 파일 오픈    

    if((pDestFile = fopen(destFile, "wb")) == NULL ){

        printf("dest file open Error! \n");

        exit(0);;

    }

 

    // 압축 파일 오픈

    if ((pOriFile = gzopen(oriFile, "rb")) == NULL){

        printf("original file open Error! \n");

        exit(0);

    }

    

    // 압축을 풀어, 새 파일에 저장

    while( curRead = gzread(pOriFile, buf, BUF_MAX) )

    {

        if ( fwrite(buf , sizeof(char), curRead, pDestFile) < 0 ){

 

            gzclose(pOriFile);

            fclose(pDestFile);

 

            exit(0);

        }

 

        //printf("%d ", strlen(buf));

        totalRead += curRead;

    }

    

    //printf("total = %d\n", totalRead);


 

    gzclose(pOriFile);

    fclose(pDestFile);

}

































































void dram_t::print( FILE* simFile) const
{
   unsigned i;
  // if(gzwrite(zfp,buf,(bufcnt-3)*sizeof(long long int)))
  // {

   //printf("gzip write final error");
  // exit(0);
  // }
//   compress("trace"); 
//   uncompress("trace.gz");
//   gzclose(zfp);
   fprintf(simFile,"DRAM[%d]: %d bks, busW=%d BL=%d CL=%d, ", 
           id, m_config->nbk, m_config->busW, m_config->BL, m_config->CL );
   fprintf(simFile,"tRRD=%d tCCD=%d, tRCD=%d tRAS=%d tRP=%d tRC=%d\n",
           m_config->tRRD, m_config->tCCD, m_config->tRCD, m_config->tRAS, m_config->tRP, m_config->tRC );
   fprintf(simFile,"n_cmd=%d n_nop=%d n_act=%d n_pre=%d n_ref_event=%d n_req=%d n_rd=%d n_rd_L2_A=%d n_write=%d n_wr_bk=%d bw_util=%.4g\n",
           n_cmd, n_nop, n_act, n_pre, n_ref, n_req, n_rd, n_rd_L2_A, n_wr, n_wr_WB,
           (float)bwutil/n_cmd);
   fprintf(simFile,"n_activity=%d dram_eff=%.4g\n",
           n_activity, (float)bwutil/n_activity);

   fprintf(simFile,"RFC = %d REFi = %d\n",m_config->tRFC, m_config->tREFi);
   for (i=0;i<m_config->nbk;i++) {
      fprintf(simFile, "bk%d: %da %di ",i,bk[i]->n_access,bk[i]->n_idle);
      fprintf(simFile, "\nRFC : %d REFi %d\n",bk[i]->RFC,bk[i]->REFi);
   }
   fprintf(simFile, "\n");
   fprintf(simFile, "\n------------------------------------------------------------------------\n");
	


   if(Veri==0)
   {
   /// printf ////
	printf("--------------------------Total cycle imformation ------------------------------");
	printf("\nSum_Cycle = %d total_cnt(row issue) = %d avg_inter_cycle= %d \n",Sum_cycle,total_cnt,Sum_cycle/total_cnt);
	printf("---------------------------------------------------------------------------------");
	Veri = 1;
   }
   printf("\nRow_Buffer_Locality = %.6f", (float)hits_num / access_num);
   printf("\nRow_Buffer_Locality_read = %.6f", (float)hits_read_num / read_num);
   printf("\nRow_Buffer_Locality_write = %.6f", (float)hits_write_num / write_num);
   printf("\nBank_Level_Parallism = %.6f", (float)banks_1time / banks_acess_total);
   printf("\nBank_Level_Parallism_Col = %.6f", (float)banks_time_rw / banks_access_rw_total);
   printf("\nBank_Level_Parallism_Ready = %.6f", (float)banks_time_ready /banks_access_ready_total);
   printf("\nwrite_to_read_ratio_blp_rw_average = %.6f", write_to_read_ratio_blp_rw_average /banks_access_rw_total);
   printf("\nGrpLevelPara = %.6f \n", (float)bkgrp_parallsim_rw /banks_access_rw_total);

   printf("\nBW Util details:\n");
   printf("bwutil = %.6f \n", (float)bwutil/n_cmd);
   printf("total_CMD = %d \n", n_cmd);
   printf("util_bw = %d \n", util_bw);
   printf("Wasted_Col = %d \n", wasted_bw_col);
   printf("Wasted_Row = %d \n", wasted_bw_row);
   printf("Idle = %d \n", idle_bw);

   printf("\nBW Util Bottlenecks: \n");
   printf("RCDc_limit = %d \n", RCDc_limit);
   printf("RCDWRc_limit = %d \n", RCDWRc_limit);
   printf("WTRc_limit = %d \n", WTRc_limit);
   printf("RTWc_limit = %d \n", RTWc_limit);
   printf("CCDLc_limit = %d \n", CCDLc_limit);
   printf("rwq = %d \n", rwq_limit);
   printf("CCDLc_limit_alone = %d \n", CCDLc_limit_alone);
   printf("WTRc_limit_alone = %d \n", WTRc_limit_alone);
   printf("RTWc_limit_alone = %d \n", RTWc_limit_alone);

   printf("\nCommands details: \n");
   printf("total_CMD = %d \n", n_cmd);
   printf("n_nop = %d \n", n_nop);
   printf("Read = %d \n", n_rd);
   printf("Write = %d \n",n_wr);
   printf("L2_Alloc = %d \n", n_rd_L2_A);
   printf("L2_WB = %d \n", n_wr_WB);
   printf("n_act = %d \n", n_act);
   printf("n_pre = %d \n", n_pre);
   printf("n_ref = %d \n", n_ref);
   printf("n_req = %d \n", n_req );
   printf("total_req = %d \n", n_rd+n_wr+n_rd_L2_A+n_wr_WB);

   printf("-----------------------------\n");
   printf("issued_total_row : %d\n",issued_total_row);
   printf("issued_total_col : %d\n",issued_total_col);


   printf("\nDual Bus Interface Util: \n");
   printf("issued_total_row = %lu \n", issued_total_row);
   printf("issued_total_col = %lu \n", issued_total_col);
   printf("Row_Bus_Util =  %.6f \n", (float)issued_total_row / n_cmd);
   printf("CoL_Bus_Util = %.6f \n", (float)issued_total_col / n_cmd);
   printf("Either_Row_CoL_Bus_Util = %.6f \n",  (float)issued_total / n_cmd);
   printf("Issued_on_Two_Bus_Simul_Util = %.6f \n",  (float)issued_two /n_cmd);
   printf("issued_two_Eff = %.6f \n",  (float)issued_two /issued_total);
   printf("queue_avg = %.6f \n\n", (float)ave_mrqs/n_cmd );



   fprintf(simFile, "\n");
   fprintf(simFile, "dram_util_bins:");
   for (i=0;i<10;i++) fprintf(simFile, " %d", dram_util_bins[i]);
   fprintf(simFile, "\ndram_eff_bins:");
   for (i=0;i<10;i++) fprintf(simFile, " %d", dram_eff_bins[i]);
   fprintf(simFile, "\n");
   if(m_config->scheduler_type== DRAM_FRFCFS)
       fprintf(simFile, "mrqq: max=%d avg=%g\n", max_mrqs, (float)ave_mrqs/n_cmd);
}

void dram_t::visualize() const
{
   printf("RRDc=%d CCDc=%d mrqq.Length=%d rwq.Length=%d\n", 
          RRDc, CCDc, mrqq->get_length(),rwq->get_length());
   for (unsigned i=0;i<m_config->nbk;i++) {
      printf("BK%d: state=%c curr_row=%03x, %2d %2d %2d %2d %p ", 
             i, bk[i]->state, bk[i]->curr_row,
             bk[i]->RCDc, bk[i]->RASc,
             bk[i]->RPc, bk[i]->RCc,
             bk[i]->mrq );
      if (bk[i]->mrq)
         printf("txf: %d %d", bk[i]->mrq->nbytes, bk[i]->mrq->txbytes);
      printf("\n");
   }
   if ( m_frfcfs_scheduler ) 
      m_frfcfs_scheduler->print(stdout);
}

void dram_t::print_stat( FILE* simFile ) 
{
   fprintf(simFile,"DRAM (%d): n_cmd=%d n_nop=%d n_act=%d n_pre=%d n_ref=%d n_req=%d n_rd=%d n_write=%d bw_util=%.4g ",
           id, n_cmd, n_nop, n_act, n_pre, n_ref, n_req, n_rd, n_wr,
           (float)bwutil/n_cmd);
   fprintf(simFile, "mrqq: %d %.4g mrqsmax=%d ", max_mrqs, (float)ave_mrqs/n_cmd, max_mrqs_temp);
   fprintf(simFile, "\n");
   fprintf(simFile, "dram_util_bins:");
   for (unsigned i=0;i<10;i++) fprintf(simFile, " %d", dram_util_bins[i]);
   fprintf(simFile, "\ndram_eff_bins:");
   for (unsigned i=0;i<10;i++) fprintf(simFile, " %d", dram_eff_bins[i]);
   fprintf(simFile, "\n");
   max_mrqs_temp = 0;
}

void dram_t::visualizer_print( gzFile visualizer_file )
{
   // dram specific statistics
   gzprintf(visualizer_file,"dramncmd: %u %u\n",id, n_cmd_partial);  
   gzprintf(visualizer_file,"dramnop: %u %u\n",id,n_nop_partial);
   gzprintf(visualizer_file,"dramnact: %u %u\n",id,n_act_partial);
   gzprintf(visualizer_file,"dramnpre: %u %u\n",id,n_pre_partial);
   gzprintf(visualizer_file,"dramnreq: %u %u\n",id,n_req_partial);
   gzprintf(visualizer_file,"dramavemrqs: %u %u\n",id,
            n_cmd_partial?(ave_mrqs_partial/n_cmd_partial ):0);

   // utilization and efficiency
   gzprintf(visualizer_file,"dramutil: %u %u\n",  
            id,n_cmd_partial?100*bwutil_partial/n_cmd_partial:0);
   gzprintf(visualizer_file,"drameff: %u %u\n", 
            id,n_activity_partial?100*bwutil_partial/n_activity_partial:0);

   // reset for next interval
   bwutil_partial = 0;
   n_activity_partial = 0;
   ave_mrqs_partial = 0; 
   n_cmd_partial = 0;
   n_nop_partial = 0;
   n_act_partial = 0;
   n_pre_partial = 0;
   n_req_partial = 0;


   // dram access type classification
   for (unsigned j = 0; j < m_config->nbk; j++) {
      gzprintf(visualizer_file,"dramglobal_acc_r: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[GLOBAL_ACC_R][id][j]);
      gzprintf(visualizer_file,"dramglobal_acc_w: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[GLOBAL_ACC_W][id][j]);
      gzprintf(visualizer_file,"dramlocal_acc_r: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[LOCAL_ACC_R][id][j]);
      gzprintf(visualizer_file,"dramlocal_acc_w: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[LOCAL_ACC_W][id][j]);
      gzprintf(visualizer_file,"dramconst_acc_r: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[CONST_ACC_R][id][j]);
      gzprintf(visualizer_file,"dramtexture_acc_r: %u %u %u\n", id, j, 
               m_stats->mem_access_type_stats[TEXTURE_ACC_R][id][j]);
   }
}


void dram_t::set_dram_power_stats(	unsigned &cmd,
									unsigned &activity,
									unsigned &nop,
									unsigned &act,
									unsigned &pre,
									unsigned &rd,
									unsigned &wr,
									unsigned &req) const{

	// Point power performance counters to low-level DRAM counters
	cmd = n_cmd;
	activity = n_activity;
	nop = n_nop;
	act = n_act;
	pre = n_pre;
	rd = n_rd;
	wr = n_wr;
	req = n_req;
}

unsigned dram_t::get_bankgrp_number(unsigned i)
{
	if(m_config->dram_bnkgrp_indexing_policy == HIGHER_BITS) { //higher bits
		return i>>m_config->bk_tag_length;
	}
	else if (m_config->dram_bnkgrp_indexing_policy == LOWER_BITS) { //lower bits
		return i&((m_config->nbkgrp-1));
	}
	else {
		assert(1);
	}
}

















