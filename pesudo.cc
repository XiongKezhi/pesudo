void main() {
	raft := Raft{} // initial a Raft object
	while true {
		event := receive()
		switch event.type {
			case UserRequest:
				raft.handleUserRequest(event.payload)
			case LogChanges:
				raft.handleLogChange(event.payload)
			case RaftRpc:
				raft.handleRaftRpc(event.payload)
		}
	}
}

void Raft::handleUserRequest(req) {
	if this.role != LEADER {
		return
	}

	swtich req.type {
		case Proposal:
			this.logs.append(Entry{type: Proposal, proposal: req})

		case SplitEnterRequest: // A request to enter joint consensus for spliting.
			// req.cfgs: desired configs after split
			// this.cfg: current config
			this.logs.append(Entry{type: SplitEnterJoint, configs: req.cfgs, prev: this.cfg})

		case SplitLeaveRequest: // A request to leave joint consensus for spliting
			if (in joint config) && (the SplitEnterJoint entry is committed) {
				// this.cfg: current config
				this.logs.append(Entry{type: SplitLeaveJoint, prev: this.cfg})
			}

		case MergeRequest: // A request to merge
			// Since we are using 2 phase commit to merge clusters,
			// in code, we view it as a transaction. And this cluster
			// will be the tx coordinator.

			// req.clusters contain the clusters to merge
			epoches := epoch numbers collected from req.clusters
			this.logs.append(Entry{type: MergePrepare, txid: uuid(), coordinator: this.config,
				clusters: req.clusters, epoches: epoches})

		case MergePrepare: // A prepare message in 2 phase commit for merging
			this.logs.append(Entry{type: MergePrepare, txid: req.txid, coordinator: req.coordinator,
				clusters: req.clusters, epoches: epoches})

		case MergePrepareResp: 
			// based on history responses and this one,
			// decide the fate of the transaction specified by req.txid
			switch (the fate of the transaction specified by request) {
				case COMMIT:
					this.logs.append(Entry{type: MergeCommit, txid: req.txid})
				case ABORT:
					this.logs.append(Entry{type: MergeAbort, txid: req.txid})
				default: // unknown yet
					addToHistory(msg) // add this response to history for future lookup
			}

		case MergeCommit:  // A commit message in 2 phase commit for merging
			this.logs.append(Entry{type: MergeCommit, txid: req.txid})

		case MergeAbort:   // An abort message in 2 phase commit for merging
			this.logs.append(Entry{type: MergeAbort, txid: req.txid})
	}
}

void Raft::handleLogChanges(change) {
	entry := change.entry
	switch change.type {
		case Append:
			switch entry.type {
				case SplitEnterJoint:
					applyConfig(entry.configs)
				case SplitLeaveJoint:
					applyConfig(in current joint configs, the one containing this node)
			}

			for node in this.config {
				sendRpc(type=AppendEntries, to=node, entry=entry, ...) 
			}

		case Commit:
		 	switch entry.type {
				case Proposal:
					applyProposal(entry.proposal)
				case SplitLeaveJoint:
					this.epoch = entry.epoch+1
				case MergePrepare:
					registered := true
					if (no ongoing merge) {
						// register and store the tx info somewhere
						storeTx(req.txid, entry)
					} else {
						registered = false
					}

					if entry.coordinator == this.config {
						if registered {
							// if this cluster is the tx coordinator
							// and successfully register the tx,
							// send prepare message to other clusters
							for clr in entry.clusters {
								if clr == this.config {
									continue
								}
								for node in clr {
									// resend until succeed
									sendRequest(type=MergePrepare, to=node, txid=entry.txid, coordinator: this.config,
												clusters: req.clusters, epoches: epoches)
								}
							}
						}
					} else {
						// else, send register result back to the coordinator
						for node in entry.coordinator {
								// resend until succeed
								sendRequest(type=MergePrepareResp, to=node, txid=entry.txid, ok=registered)
						}
					}
				case MergeCommit:
					info := retrieveTx(entry.txid)
					commitTx(entry.txid)

					// use clusters as a joint consensus as the start of the new epoch
					applyConfig(a joint config consist of entry.clusters)

					// convert to follower so no multiple leaders at the same time
					this.role = FOLLOWER
					this.epoch = max(info.epoches)
					this.term = 0
				
					// propogate result to others if this cluster is the coordinator
					if info.coordinator == this.config {
						for clr in entry.clusters {
								if clr == this.config {
									continue
								}
								for node in clr {
									// resend until succeed
									sendRequest(type=MergeCommit, to=node, txid=entry.txid)
								}
						}
					}
				
					// take a special snapshot which contains only the state machine data
					takeStateMachineSnapshot()

					// append an MergeSnapshot entry which serves for 2 purposes:
					// 1. hide the inconsistency of logs between clusters before this point
					// 2. require nodes to apply snapshots from others clusters so their 
					//    state machine become the same.
					logs.append(Entry{type: MergeSnapshot, clusters: entry.clusters,
						epoch: this.epoch, term: 0, index: 0})

				case MergeSnapshot:
					clusters := entry.clusters

					snapshots := search locally for state machine snapshots from nodes originated from different clusters
					if snapshots == nil { // not requested yet
						request snapshots from nodes originated from different clusters
					}
					for ss in snapshots {
						mergeStateMachineSnapshot(ss)
					}

				case MergeAbort:
					info := retrieveTx(entry.txid)
					abortTx(entry.txid)

					// propogate result to others if this cluster is the coordinator
					if info.coordinator == this.config {
						for clr in entry.clusters {
								if clr == this.config {
									continue
								}
								for node in clr {
									// resend until succeed
									sendRequest(type=MergeAbort, to=node, txid=entry.txid)
								}
						}
					}
			}

		case Overwritten: // A log entry is overwritten
			if entry.type == SplitEnterjoint ||
				entry.type == SplitLeaveJoint {
				// recover to previous config
				applyConfig(entry.prev)
			}
	} 
}

void Raft::handleRaftRpc(msg) {
	if msg.epoch != this.epoch {
		handleDiffEpochRpc(msg)
	} else {
		handleSamEpochRpc(msg)
	}
}

void Raft::handleDiffEpochRpc(msg) {
	switch msg.Type {
		// Rule of election: never vote for nodes from different epoches.
		case RequestVote:
			if msg.epoch < this.epoch {
				// From lower epoch: reject with our own epoch,
				// so the candidate will know it was out-of-date.
				sendRpc(type=RequestVoteResp, to=msg.from, epoch=this.epoch,
					term=this.term, voteGranted=false)
			} else {
				// From higher epoch: reject with our own epoch,
				// Vote for higher epoch can impede progress to commit the transaction
				// during merge, see corner case <>
				sendRpc(type=RequestVoteResp, to=msg.from, epoch=this.epoch,
					term=this.term, voteGranted=false)
			}
		case RequestVoteResp:
			if msg.epoch < this.epoch {
				// From lower epoch: ignore, out-of-date response.
			} else {
				// From higher epoch: rejected due to higher epoch,
				// so we can trigger pull entries RPC.
				sendRpc(type=PullEntries, to=msg.from, epoch=this.epoch,
					term=this.term, pullFromIndex=this.commitIndex+1)
				becomeFollower()
			}

		case AppendEntries:
			if msg.epoch < this.epoch {
				// From lower epoch: reject with our own epoch,
				sendRpc(type=AppendEntriesResp, to=msg.from, epoch=this.epoch,
					term=this.term, success=false)
			} else {
				// From higher epoch: accept based on prev-log match rule (the original raft way).
				// When an `AppendEntries` rpc is issued from a higher epoch, meaning a leader
				// has been elected there, then this node must be a delayed one.
				entry := this.logs.getByIndex(msg.prevLogIndex)
				if entry.epoch == msg.prevLogEpoch && entry.term == msg.prevLogTerm {
					conflictIndex := conflictIndex between logs in this node and msg
					this.logs.removeSince(conflictIndex)
					this.logs.append(msg.entries)
					this.commitIndex = max(msg.leaderCommit, this.commitIndex)

					sendRpc(type=AppendEntriesResp, to=msg.from, epoch=this.epoch,
						term=this.term, success=true)
				} else {
					sendRpc(type=AppendEntriesResp, to=msg.from, epoch=this.epoch,
						term=this.term, success=true)
				}

				becomeFollower()
			}
		case AppendEntriesResp:
			if this.role != LEADER {
				return
			}

			if msg.epoch < this.epoch {
				// From lower epoch: handle as normal raft.
				if msg.success {
					this.matchIndex[this.nextIndex[msg.from]]
					if (there exists an N such that
						1. N > commitIndex, a majority of matchIndex[i] >= N, and 
						2. log.getByIndex(N).term == currentTerm) {
						oldCommitIndex = this.commitIndex
						this.commitIndex = N
						commitEntries(this.logs.getBetween(oldCommitIndex, this.commitIndex))
					}
				} else {
					if this.nextIndex[msg.from] > this.snapshotIndex { // send logs
						this.nextIndex[msg.from] -= 1
						prevLog := this.log.getByIndex(this.nextIndex[msg.from] - 1)
						sendRpc(type=AppendEntries, to=msg.from, 
							epoch=this.epoch, term=this.term, leaderId=this.id, 
							prevLogEpoch=prevLog.epoch, prevLogIndex=prevLog.index, prevLogTerm=prevLog.term,
							entries=this.logs.getSinceIndex(prevLog.index+1), leaderCommit=this.commitIndex)
					} else { // send snapshot
						sendRpc(type=Snapshot, to=msg.from, snapshot=this.snapshot)
					}
				}
			} else {
				// From higher epoch: happens in one case, this node is a stale leader in
				// a stale epoch.
				sendRpc(type=PullEntries, to=msg.from, epoch=this.epoch,
					term=this.term, pullFromIndex=this.commitIndex+1)
				becomeFollower()
			}

		case PullEntries:
			if msg.epoch < this.epoch {
				// From lower epoch: send logs or snapshot (if compacted).
				if msg.pullFromIndex < this.logs[0].index { // some logs have been compacted
					sendRpc(type=PullEntriesResp, to=msg.from, epoch=this.epoch,
						term=this.term, snapshot=this.snapshot)
					break
				}
				
				sendRpc(type=PullEntriesResp, to=msg.from, epoch=this.epoch,
					term=this.term, entries=this.logs.getBetween(msg.pullInexFrom, this.committedIndex))
			} else {
				// From higher epoch: impossible
				panic()
			}
		case PullEntriesResp:
			if msg.epoch < this.epoch {
				// From lower epoch: must be an out-of-date response, ignore
			} else {
				// From higher epoch: append to log (resolve conflict entries if necessary)
				if msg.snapshot != null {
					applySnapshot(msg.snapsot)
				} else {
					conflictIndex := conflictIndex between logs in this node and msg
					this.logs.removeSince(conflictIndex)
					this.logs.append(msg.entries)
					
					oldCommitIndex := this.commitIndex
					this.commitIndex = msg.entries[len(msg.entries)-1].index
					commitEntries(this.logs.getBetween(oldCommitIndex, this.commitIndex))
				}
			}

		case Snapshot:
			if msg.epoch < this.epoch {
				// From lower epoch: ignore
			} else {
				// From higher epoch: apply
				applySnapshot(msg.snapshot)
			}
	}
}

void handleSameEpochRpc(msg) {
	// this function will be the same to original Raft logic
}
