/*
 * This file is part of SwabianCoin.
 *
 * SwabianCoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * SwabianCoin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SwabianCoin.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "BlockchainManager.h"
#include <functional>

using namespace scn;

SynchronizedTimer BlockchainManager::static_sync_timer_;

BlockchainManager::BlockchainManager(const public_key_t& our_public_key,
                                     const private_key_t& our_private_key,
                                     Blockchain& blockchain,
                                     IP2PConnector& p2p_connector,
                                     IMiner& miner,
                                     bool initial_fetch,
                                     ISynchronizedTimer& sync_timer)
: our_public_key_(our_public_key)
, our_private_key_(our_private_key)
, sync_timer_(sync_timer)
, crypto_(our_private_key)
, blockchain_(blockchain)
, p2p_connector_(p2p_connector)
, miner_(miner)
, initial_fetch_(initial_fetch)
, out_of_sync_detector_(sync_timer_)
, peers_monitor_(sync_timer_, p2p_connector_)
, active_peers_collector_(p2p_connector_, our_public_key_)
, cycle_state_fetch_blockchain_(*this)
, cycle_state_collect_(*this)
, cycle_state_introduce_block_(*this)
, cycle_state_introduce_baseline_(*this)
, current_state_(nullptr)
, running_(true)
, update_state_thread_(nullptr) {
    p2p_connector_.registerBlockCallbacks(std::bind(&BlockchainManager::baselineBlockReceivedCallback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3),
                                          std::bind(&BlockchainManager::collectionBlockReceivedCallback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    p2p_connector_.connect();

    update_state_thread_ = std::make_unique<std::thread>(&BlockchainManager::updateStateThread, this);
}


BlockchainManager::~BlockchainManager() {
    p2p_connector_.disconnect();
    running_ = false;
    update_state_thread_->join();
}


void BlockchainManager::baselineBlockReceivedCallback(const peer_id_t& peer_id, std::shared_ptr<const BaselineBlock> block, bool reply) {
    LOCK_MUTEX_WATCHDOG_REC(mtx_current_state_access_);
    if(current_state_ != nullptr) {
        current_state_->blockReceivedCallback(peer_id, block, reply);
    }

}


void BlockchainManager::collectionBlockReceivedCallback(const peer_id_t& peer_id, std::shared_ptr<const CollectionBlock> block, bool reply) {
    LOCK_MUTEX_WATCHDOG_REC(mtx_current_state_access_);
    if(current_state_ != nullptr) {
        current_state_->blockReceivedCallback(peer_id, block, reply);
    }
    peers_monitor_.blockReceivedCallback(peer_id, *block, reply);
}


void BlockchainManager::foundHashCallback(epoch_t epoch, const std::string& data) {
    LOCK_MUTEX_WATCHDOG(mtx_found_hash_queue_access_);
    found_hash_queue_.push(data);
}


void BlockchainManager::triggerTransaction(const public_key_t& receiver, uint64_t fraction) {
    LOCK_MUTEX_WATCHDOG(mtx_transaction_queue_access_);
    transaction_queue_.push(std::pair<const public_key_t,uint64_t>(receiver, fraction));
}


void BlockchainManager::join() {
    update_state_thread_->join();
}


void BlockchainManager::pauseMiner() {
    LOCK_MUTEX_WATCHDOG(mtx_control_miner_);
    miner_.stop();
}


void BlockchainManager::resumeMiner() {
    auto mining_state = blockchain_.getMiningState();
    LOCK_MUTEX_WATCHDOG(mtx_control_miner_);
    miner_.start(mining_state.highest_hash_of_last_epoch, our_public_key_, mining_state.epoch, std::bind(&BlockchainManager::foundHashCallback, this, std::placeholders::_1, std::placeholders::_2));
}


uint8_t BlockchainManager::percentBlockchainSynchronized() const {
    LOCK_MUTEX_WATCHDOG_REC(mtx_current_state_access_);
    if(current_state_ == &cycle_state_fetch_blockchain_) {
        return cycle_state_fetch_blockchain_.percentSynchronizationDone();
    } else if(current_state_ == nullptr) {
        return 0;
    } else {
        return 100;
    }
}


uint64_t BlockchainManager::getTotalPeersEstimation() const {
    return active_peers_collector_.getActivePeers();
}


ICycleState::State BlockchainManager::getCurrentState() const {
    LOCK_MUTEX_WATCHDOG_REC(mtx_current_state_access_);
    if(current_state_ == nullptr) {
        return ICycleState::State::Unassigned;
    } else {
        return current_state_->getState();
    }
}


bool BlockchainManager::isBaselineBlock(block_uid_t block_uid) {
    return ((block_uid-1) % max_num_blocks_after_baseline_) == 0;
}


block_uid_t BlockchainManager::getNextBaselineBlock(block_uid_t block_uid) {
    return (((block_uid-1) / max_num_blocks_after_baseline_) + 1) * max_num_blocks_after_baseline_ + 1;
}


block_uid_t BlockchainManager::getPreviousBaselineBlock(block_uid_t block_uid) {
    return std::max(static_cast<block_uid_t>(max_num_blocks_after_baseline_ + 1), getNextBaselineBlock(block_uid)) - max_num_blocks_after_baseline_;
}


block_uid_t BlockchainManager::getBlockId(blockchain_time_t blockchain_time) {
	blockchain_time_t div = blockchain_time / cycle_length_ms_;
    return div - std::min(div, static_cast<blockchain_time_t>(12892961ul));
}


uint32_t BlockchainManager::setState(ICycleState& new_state) {
    std::chrono::time_point<std::chrono::system_clock> t0, t1;
    t0 = std::chrono::system_clock::now();

    {
        LOCK_MUTEX_WATCHDOG_REC(mtx_current_state_access_);
        if (&new_state != current_state_) {
            if (current_state_ != nullptr) {
                current_state_->onExit();
            }

            current_state_ = &new_state;

            if (current_state_ != nullptr) {
                current_state_->onEnter();
            }
        }
    }

    t1 = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}


bool BlockchainManager::cycleUntil(blockchain_time_t target_time) {
    while(running_ && sync_timer_.now() < target_time) {
        bool do_sleep = true;
        {
            LOCK_MUTEX_WATCHDOG_REC(mtx_current_state_access_);
            if (current_state_ != nullptr) {
                do_sleep = !current_state_->onCycle();
            }
        }

        if (do_sleep) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    return running_;
}


void BlockchainManager::updateStateThread() {

    if(initial_fetch_) {
        fetchBlockchain();
    }

    resumeMiner();
    active_peers_collector_.restartListBuilding();

    auto time_next_state_change = (sync_timer_.now() / cycle_length_ms_) * cycle_length_ms_ + (cycle_length_ms_/4);

    while(running_) {
        bool time_violation_detected = false;

        if(setState(cycle_state_introduce_block_) > cycle_length_ms_) {
            time_violation_detected = true;
        }

        time_next_state_change += (cycle_length_ms_*3/4);
        if(!cycleUntil(time_next_state_change)) {
            break;
        }

        if(isBaselineBlock(blockchain_.getNewestBlockId()+2)) {
            if(setState(cycle_state_introduce_baseline_) > cycle_length_ms_) {
                time_violation_detected = true;
            }

            time_next_state_change += cycle_length_ms_;
            if(!cycleUntil(time_next_state_change)) {
                break;
            }
        }

        if(setState(cycle_state_collect_) > cycle_length_ms_) {
            time_violation_detected = true;
        }

        time_next_state_change += (cycle_length_ms_/4);
        if(!cycleUntil(time_next_state_change)) {
            break;
        }

        if(out_of_sync_detector_.isOutOfSync() || time_violation_detected) {
            LOG(ERROR) << "Blockchain out of sync detected!" << (time_violation_detected ? " Time violation." : "");
            fetchBlockchain();
            active_peers_collector_.restartListBuilding();
            time_next_state_change = (sync_timer_.now() / cycle_length_ms_) * cycle_length_ms_ + (cycle_length_ms_/4);
        }
    }

    {
        LOCK_MUTEX_WATCHDOG_REC(mtx_current_state_access_);
        if(current_state_ != nullptr) {
            current_state_->onExit();
            current_state_ = nullptr;
        }
    }
}


void BlockchainManager::fetchBlockchain() {
    setState(cycle_state_fetch_blockchain_);
    while (running_ && !cycle_state_fetch_blockchain_.isSynchronized()) {
        bool do_sleep = true;
        {
            LOCK_MUTEX_WATCHDOG_REC(mtx_current_state_access_);
            if(current_state_ != nullptr) {
                do_sleep = !current_state_->onCycle();
            }
        }

        if(do_sleep) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}