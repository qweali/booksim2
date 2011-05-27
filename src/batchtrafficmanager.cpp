// $Id$

/*
Copyright (c) 2007-2011, Trustees of The Leland Stanford Junior University
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this 
list of conditions and the following disclaimer in the documentation and/or 
other materials provided with the distribution.
Neither the name of the Stanford University nor the names of its contributors 
may be used to endorse or promote products derived from this software without 
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND 
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR 
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON 
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS 
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <limits>
#include <sstream>
#include <fstream>

#include "packet_reply_info.hpp"
#include "random_utils.hpp"
#include "batchtrafficmanager.hpp"

BatchTrafficManager::BatchTrafficManager( const Configuration &config, 
					  const vector<Network *> & net )
: TrafficManager(config, net), _last_id(-1), _last_pid(-1), 
   _overall_min_batch_time(0), _overall_avg_batch_time(0), 
   _overall_max_batch_time(0)
{

  _max_outstanding = config.GetInt ("max_outstanding_requests");  

  _batch_size = config.GetInt( "batch_size" );
  _batch_count = config.GetInt( "batch_count" );

  _batch_time = new Stats( this, "batch_time", 1.0, 1000 );
  _stats["batch_time"] = _batch_time;
  
  string sent_packets_out_file = config.GetStr( "sent_packets_out" );
  if(sent_packets_out_file == "") {
    _sent_packets_out = NULL;
  } else {
    _sent_packets_out = new ofstream(sent_packets_out_file.c_str());
  }
}

BatchTrafficManager::~BatchTrafficManager( )
{
  delete _batch_time;
  if(_sent_packets_out) delete _sent_packets_out;
}

void BatchTrafficManager::_RetireFlit( Flit *f, int dest )
{
  _last_id = f->id;
  _last_pid = f->pid;
  TrafficManager::_RetireFlit(f, dest);
}

int BatchTrafficManager::_IssuePacket( int source, int cl )
{
  int result = 0;
  if(_use_read_write[cl]) { //read write packets
    //check queue for waiting replies.
    //check to make sure it is on time yet
    if(!_repliesPending[source].empty()) {
      if(_repliesPending[source].front()->time <= _qtime[source][cl]) {
	result = -1;
      }
    } else {
      if((_sent_packets[source] < _batch_size) && 
	 ((_max_outstanding <= 0) || 
	  (_requestsOutstanding[source] < _max_outstanding))) {
	
	//coin toss to determine request type.
	result = (RandomFloat() < 0.5) ? 2 : 1;
      
	_requestsOutstanding[source]++;
      }
    }
  } else { //normal
    if((_sent_packets[source] < _batch_size) && 
       ((_max_outstanding <= 0) || 
	(_requestsOutstanding[source] < _max_outstanding))) {
      result = _packet_size[cl];
      _requestsOutstanding[source]++;
    }
  }
  if(result != 0) {
    _sent_packets[source]++;
  }
  return result;
}

void BatchTrafficManager::_ClearStats( )
{
  TrafficManager::_ClearStats();
  _batch_time->Clear( );
}

bool BatchTrafficManager::_SingleSim( )
{
  int batch_index = 0;
  while(batch_index < _batch_count) {
    _sent_packets.assign(_nodes, 0);
    _last_id = -1;
    _last_pid = -1;
    _sim_state = running;
    int start_time = _time;
    bool batch_complete;
    do {
      _Step();
      batch_complete = true;
      for(int i = 0; i < _nodes; ++i) {
	if(_sent_packets[i] < _batch_size) {
	  batch_complete = false;
	  break;
	}
      }
      if(_sent_packets_out) {
	*_sent_packets_out << _sent_packets << ";" << endl;
      }
    } while(!batch_complete);
    cout << "Batch " << batch_index + 1 << " ("<<_batch_size  <<  " packets) sent. Time used is " << _time - start_time << " cycles." << endl;
    cout << "Draining the Network...................\n";
    _sim_state = draining;
    _drain_time = _time;
    int empty_steps = 0;
    
    bool packets_left = false;
    for(int c = 0; c < _classes; ++c) {
      packets_left |= !_total_in_flight_flits[c].empty();
    }
    
    while( packets_left ) { 
      _Step( ); 
      
      ++empty_steps;
      
      if ( empty_steps % 1000 == 0 ) {
	_DisplayRemaining( ); 
	cout << ".";
      }
      
      packets_left = false;
      for(int c = 0; c < _classes; ++c) {
	packets_left |= !_total_in_flight_flits[c].empty();
      }
    }
    cout << endl;
    cout << "Batch " << batch_index + 1 << " ("<<_batch_size  <<  " packets) received. Time used is " << _time - _drain_time << " cycles. Last packet was " << _last_pid << ", last flit was " << _last_id << "." <<endl;

    _batch_time->AddSample(_time - start_time);

    cout << _sim_state << endl;

    DisplayStats();
        
    ++batch_index;
  }
  return 1;
}

void BatchTrafficManager::_UpdateOverallStats() {
  TrafficManager::_UpdateOverallStats();
  _overall_min_batch_time += _batch_time->Min();
  _overall_avg_batch_time += _batch_time->Average();
  _overall_max_batch_time += _batch_time->Max();
}
  
string BatchTrafficManager::_OverallStatsCSV(int c) const
{
  ostringstream os;
  os << TrafficManager::_OverallStatsCSV(c) << ','
     << _overall_min_batch_time / (double)_total_sims << ','
     << _overall_avg_batch_time / (double)_total_sims << ','
     << _overall_max_batch_time / (double)_total_sims;
  return os.str();
}

void BatchTrafficManager::WriteStats(ostream & os) const
{
  os << "batch_time = " << _batch_time->Average() << ";" << endl;
  TrafficManager::WriteStats(os);
}    

void BatchTrafficManager::DisplayStats(ostream & os) const {
  os << "Minimum batch duration = " << _batch_time->Min() << endl;
  os << "Average batch duration = " << _batch_time->Average() << endl;
  os << "Maximum batch duration = " << _batch_time->Max() << endl;
  TrafficManager::DisplayStats();
}

void BatchTrafficManager::DisplayOverallStats(ostream & os) const {
  TrafficManager::DisplayOverallStats(os);
  os << "Overall min batch duration = " << _overall_min_batch_time / (double)_total_sims
     << " (" << _total_sims << " samples)" << endl
     << "Overall min batch duration = " << _overall_avg_batch_time / (double)_total_sims
     << " (" << _total_sims << " samples)" << endl
     << "Overall min batch duration = " << _overall_max_batch_time / (double)_total_sims
     << " (" << _total_sims << " samples)" << endl;
}
