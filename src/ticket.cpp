#include <ticket.h>
#include <vector>

using namespace std;

CScript GenerateTicketScript(const CPubKey keyid, const int lockHeight)
{
    //auto script = CScript() << CScriptNum(lockHeight) << OP_CHECKLOCKTIMEVERIFY << OP_DROP << OP_DUP << OP_HASH160 << ToByteVector(keyid) << OP_EQUALVERIFY << OP_CHECKSIG;
    auto script = CScript() << CScriptNum(lockHeight) << OP_CHECKLOCKTIMEVERIFY << OP_DROP << ToByteVector(keyid) << OP_CHECKSIG;
    return std::move(script);
}

bool GetPublicKeyFromScript(const CScript script, CPubKey &pubkey)
{
    CScriptBase::const_iterator pc = script.begin();
    opcodetype opcodeRet;
    vector<unsigned char> vchRet;
    if (script.GetOp(pc, opcodeRet, vchRet) && CScriptNum(vchRet,true)> 0) {
        vchRet.clear();
        if (script.GetOp(pc, opcodeRet, vchRet) && opcodeRet == OP_CHECKLOCKTIMEVERIFY) {
            vchRet.clear();
            if (script.GetOp(pc, opcodeRet, vchRet) && opcodeRet == OP_DROP) {
                vchRet.clear();
                if (script.GetOp(pc, opcodeRet, vchRet) && vchRet.size() == 33) {
                    pubkey = CPubKey(vchRet);
                    return true;
                }
            }
        }
    }
    return false;
}