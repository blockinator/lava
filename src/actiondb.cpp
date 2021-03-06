#include <actiondb.h>
#include <validation.h>
#include <chainparams.h>
#include <logging.h>
#include <key_io.h>
#include <algorithm>

CAction MakeBindAction(const CKeyID& from, const CKeyID& to)
{
    CBindAction ba(std::make_pair(from, to));
    return std::move(CAction(ba));
}

bool SignAction(const COutPoint& out, const CAction &action, const CKey& key, std::vector<unsigned char>& vch)
{
    vch.clear();
    auto actionVch = SerializeAction(action);
    vch.insert(vch.end(), actionVch.begin(), actionVch.end());
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << actionVch << out;
    std::vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig)) {
        return false;
    }
    vch.insert(vch.end(), vchSig.begin(), vchSig.end());
    return true;
}

bool VerifyAction(const COutPoint& out, const CAction& action, std::vector<unsigned char>& vchSig)
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << SerializeAction(action) << out;
    CPubKey pubkey;
    if (!pubkey.RecoverCompact(ss.GetHash(), vchSig))
        return false;
    auto result{ false };
    if (action.type() == typeid(CBindAction)) {
        auto from = boost::get<CBindAction>(action).first;
        result = from == pubkey.GetID();
    } else if (action.type() == typeid(CUnbindAction)) {
        auto from = boost::get<CUnbindAction>(action);
        result = from == pubkey.GetID();
    }
    return result;
}

std::vector<unsigned char> SerializeAction(const CAction& action) {
    CDataStream ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << action.which();
    boost::apply_visitor(CActionVisitor(&ss), action);
    return std::vector<unsigned char>(ss.begin(), ss.end());
}

CAction UnserializeAction(const std::vector<unsigned char>& vch) {
    CDataStream ss(vch, SER_GETHASH, PROTOCOL_VERSION);
    int ty = 0;
    ss >> ty;
    switch (ty) {
    case 1:
    {
        CBindAction ba;
        ss >> ba;
        return std::move(CAction(ba));
    }
    case 2:
    {
        CUnbindAction uba;
        ss >> uba;
        return std::move(CAction(uba));
    }
    }
    return std::move(CAction(CNilAction{}));
}

CAction DecodeAction(const CTransactionRef& tx, std::vector<unsigned char>& vchSig)
{
    do {
        if (tx->IsCoinBase() || tx->IsNull() || tx->vout.size() != 2 
            || (tx->vout[0].nValue != 0 && tx->vout[1].nValue != 0)) 
            continue;

        CAmount nAmount{ 0 };
        for (auto vin : tx->vin) {
            auto coin = pcoinsTip->AccessCoin(vin.prevout);
            nAmount += coin.out.nValue;
        }
        auto outValue = tx->GetValueOut();
        if (nAmount - outValue != Params().GetConsensus().nActionFee) {
            LogPrintf("Action warning fees, fee=%u\n", nAmount - outValue);
            continue;
        }
        for (auto vout : tx->vout) {
            if (vout.nValue != 0) continue;
            auto script = vout.scriptPubKey;
            CScriptBase::const_iterator pc = script.begin();
            opcodetype opcodeRet;
            std::vector<unsigned char> vchRet;
            if (!script.GetOp(pc, opcodeRet, vchRet) || opcodeRet != OP_RETURN) {
                continue;
            }
            script.GetOp(pc, opcodeRet, vchRet);
            auto action = UnserializeAction(vchRet);
            if (vchRet.size() < 65) continue;
            vchSig.clear();
            vchSig.insert(vchSig.end(), vchRet.end() - 65, vchRet.end());
            return std::move(action);
        }
    } while (false);
    return CAction(CNilAction{});
}


static const char DB_ACTIVE_ACTION_KEY = 'K';
static const char DB_RELATIONID = 'P';

CRelationView::CRelationView(size_t nCacheSize, bool fMemory, bool fWipe)
    : CDBWrapper(GetDataDir() / "action" / "relation", nCacheSize, fMemory, fWipe) 
{
}

CKeyID CRelationView::To(const uint160& from, uint64_t plotid, bool poc21) const
{
    if (poc21){
        // If POC21 is actived.
        auto kv = relationKeyIDTip.find(CKeyID(from));
        if(kv!=relationKeyIDTip.end()){
            return std::move(kv->second);  
        }
        return std::move(CKeyID());
    }

    CKeyID value;
    auto key = relationTip.find(plotid);
    if(key!=relationTip.end()){
        auto to_key = std::make_pair(DB_RELATIONID, key->second);
        if(!Read(to_key, value)){
            LogPrint(BCLog::RELATION, "CRelationView::To failure, can not get to plotid, from:%u\n", plotid);
        }
    }else{
        LogPrint(BCLog::RELATION, "CRelationView::To failure, get bind to, from:%u\n", plotid);
    }
    return std::move(value);
}

void CRelationView::addRelationHistory(const int height, const CKeyID& from, const CKeyID& to){
    CPersonalRelationHistoryList personalRelationList;
    auto iter = relationsHistoryMap.find(from);
    if (iter != relationsHistoryMap.end()){
        personalRelationList = relationsHistoryMap[from];
    }
    // For one person, 
    // One height only to One action
    personalRelationList[height] = to;
    relationsHistoryMap[from] = personalRelationList;
}

bool CRelationView::AcceptAction(const int height, const uint256& txid, const CAction& action, std::vector<std::pair<uint256, CRelationActive>>& relations, bool poc21)
{
    CDBBatch batch(*this);
    LogPrintf("AcceptAction, tx:%s\n", txid.GetHex());
    if (action.type() == typeid(CBindAction)) {
        auto ba = boost::get<CBindAction>(action);
        auto active = std::make_pair(txid, std::make_pair(ba.first, ba.second));
        relations.push_back(active);
        if (! poc21){
            // old poc2 need old relationMap to validate.
            // write plotID and CKeyID into disk.
            batch.Write(std::make_pair(DB_RELATIONID, ba.first.GetPlotID()), ba.first);
            batch.Write(std::make_pair(DB_RELATIONID, ba.second.GetPlotID()), ba.second);
            // add new action at tip
            relationTip[ba.first.GetPlotID()] = ba.second.GetPlotID();
            LogPrintf("bind action, from:%u, to:%u\n", ba.first.GetPlotID(), ba.second.GetPlotID());
        }
        relationKeyIDTip[ba.first] = ba.second;
        // use a cache map--personalRelationsMap to record each person relations history
        addRelationHistory(height, ba.first, ba.second);
        LogPrintf("POC2+ bind action, from address : %u, to address : %u\n", EncodeDestination(ba.first), EncodeDestination(ba.second));
    } else if (action.type() == typeid(CUnbindAction)) {
        auto from = boost::get<CUnbindAction>(action);
        auto active = std::make_pair(txid,std::make_pair(from, CKeyID()));
        relations.push_back(active);
        if (! poc21){
            LogPrintf("unbind action, from plotid:%u\n", from.GetPlotID());
            auto key = relationTip.find(from.GetPlotID());
            if(key!=relationTip.end()){
                relationTip.erase(key);
            }
        }
        LogPrintf("POC2+ unbind action, from address : %u\n", EncodeDestination(from));
        auto key = relationKeyIDTip.find(from);
        if(key!=relationKeyIDTip.end()){
            relationKeyIDTip.erase(key);
        }
        // use a cache map--personalRelationsMap to record each person relations history
        addRelationHistory(height, from, CKeyID());
    }
    return WriteBatch(batch);
}

void CRelationView::ConnectBlock(const int height, const CBlock &blk, bool poc21){
    std::vector<std::pair<uint256, CRelationActive>> relations;
    //accept action
    for (auto tx: blk.vtx) {
        std::vector<unsigned char> vchSig;
        auto action = DecodeAction(tx, vchSig);
        if (action.type() != typeid(CNilAction)) {
            LogPrintf("DecodeAction not nil action: %s\n", tx->GetHash().GetHex());
            auto out = tx->vin[0].prevout;
            if (VerifyAction(out, action, vchSig)) {
                if (!AcceptAction(height, tx->GetHash(), action, relations, poc21)) {
                    LogPrintf("AcceptAction failure: %s\n", tx->GetHash().GetHex());
                }
            }
            else {
                LogPrintf("VerifyAction failure: %s\n", tx->GetHash().GetHex());
            }
        }
    }

    if (relations.size() > 0) {
        if (!WriteRelationsToDisk(height, relations)) {
            LogPrint(BCLog::RELATION, "%s: WriteRelationToDisk retrun false, height:%d\n", __func__, height);
        }
    }
}

bool CRelationView::WriteRelationsToDisk(const int height, const std::vector<std::pair<uint256, CRelationActive>>& relations)
{
    return Write(std::make_pair(DB_ACTIVE_ACTION_KEY, height), relations);
}

bool sort_first_decline(const CPersonalHeightRelation & m1, const CPersonalHeightRelation & m2) {
    // decline sort for the height
    return m1.first > m2.first;
}

bool CRelationView::removeRelationHistory(const int height, const CKeyID& from, bool poc21){
    // remove relationsHistoryMap entry for prev relation
    CPersonalRelationHistoryList& personalRelationList = relationsHistoryMap[from];
    for (auto iter = personalRelationList.begin(); iter != personalRelationList.end(); ){
        if (iter->first >= height){
            iter = personalRelationList.erase(iter);
        }else{
            ++iter;
        }
    }

    if (personalRelationList.size() == 0){
        // the last relation has been removed,
        // so after clearing the RelationTip, we finish the work.
        relationsHistoryMap.erase(from);
        // clear the relation
        if(!poc21){
            relationTip.erase(from.GetPlotID());
        }
        relationKeyIDTip.erase(from);
        return true;
    }

    // Now, we deal with the relationTip.
    // sort and find the first prev relation
    CPersonalHeightRelationVec vec;
    copy(personalRelationList.begin(), personalRelationList.end(), std::back_inserter<CPersonalHeightRelationVec>(vec));
    sort(vec.begin(), vec.end(), sort_first_decline); 
    auto is_first_prev = [height](const CPersonalHeightRelation& personHeightRelation)->bool{
        return (height > personHeightRelation.first);
    };
    auto prevRelationIter = find_if(vec.begin(), vec.end(), is_first_prev);
    if (prevRelationIter == vec.end()){
        // clear the relation
        if(!poc21){
            relationTip.erase(from.GetPlotID());
        }
        relationKeyIDTip.erase(from);
    }else{
        // update the tip
        if(!poc21){
            relationTip[from.GetPlotID()] = prevRelationIter->second.GetPlotID();
        }
        relationKeyIDTip[from] = prevRelationIter->second;
    }
    return true;
}

void CRelationView::DisconnectBlock(const int height, const CBlock &blk, bool poc21)
{
    // erase disk
    LogPrint(BCLog::RELATION, "%s: height:%d, block:%s\n", __func__, height, blk.GetHash().ToString());
    auto key = std::make_pair(DB_ACTIVE_ACTION_KEY, height);
    Erase(key, true);

    for(auto historyEntry : relationsHistoryMap){
        auto iter = historyEntry.second.find(height);
        if (iter != historyEntry.second.end()){
            CKeyID from = historyEntry.first;
            removeRelationHistory(height, from, poc21);
        }
    }
}

bool CRelationView::LoadRelationFromDisk(const int height, bool poc21)
{
    auto key = std::make_pair(DB_ACTIVE_ACTION_KEY, height);
    if (Exists(key)) {
        std::vector<std::pair<uint256, CRelationActive>> relations;
        if (!Read(key, relations)) {
            LogPrint(BCLog::RELATION, "%s: Read retrun false, height:%d\n", __func__, height);
            return false;
        }
        for (auto relation : relations) {
            if (relation.second.second != CKeyID()) {
                auto from = relation.second.first;
                auto to   = relation.second.second;
                if (! poc21){
                    relationTip[from.GetPlotID()] = to.GetPlotID();
                    LogPrintf("bind action, from:%u, to:%u\n", from.GetPlotID(), to.GetPlotID());
                }
                relationKeyIDTip[from] = to;
                addRelationHistory(height, from, to);
                LogPrintf("POC2+ bind action, from : %u, to : %u\n", EncodeDestination(from), EncodeDestination(to));
            } else if (relation.second.second == CKeyID()) {
                auto from = relation.second.first;
                if (! poc21){
                    LogPrintf("unbind action, from:%u\n", from.GetPlotID());
                    auto key = relationTip.find(from.GetPlotID());
                    if(key!=relationTip.end()){
                        relationTip.erase(key);
                    }
                }
                LogPrintf("POC2+ unbind action, from : %u\n", EncodeDestination(from));
                auto key = relationKeyIDTip.find(from);
                if(key!=relationKeyIDTip.end()){
                    relationKeyIDTip.erase(key);
                }
                addRelationHistory(height, from, CKeyID());
            }
        }
    }
    return true;
}

CRelationVector CRelationView::ListRelations() const
{
    CRelationVector vch;
    vch.reserve(relationKeyIDTip.size());
    for (auto& iter : relationKeyIDTip) {
        vch.emplace_back(iter.first, iter.second);
    }
    return vch;
}