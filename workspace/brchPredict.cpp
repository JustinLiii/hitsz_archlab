#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <cassert>
#include <stdarg.h>
#include <cstdlib>
#include <cstring>
#include <types.h>
#include "pin.H"

using namespace std;

typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long int   UINT64;
typedef unsigned __int128   UINT128;



ofstream OutFile;

// 将val截断, 使其宽度变成bits
#define truncate(val, bits) (((UINT128)val) & (((UINT128)1 << ((UINT128)bits)) - (UINT128)1))

static UINT64 takenCorrect = 0;
static UINT64 takenIncorrect = 0;
static UINT64 notTakenCorrect = 0;
static UINT64 notTakenIncorrect = 0;

// 饱和计数器 (N < 64)
class SaturatingCnt
{
    size_t m_wid;
    UINT8 m_val;
    const UINT8 m_init_val;

    public:
        SaturatingCnt(size_t width = 2) : m_init_val((1 << width) / 2)
        {
            m_wid = width;
            m_val = m_init_val;
        }

        void increase() { if (m_val < (1 << m_wid) - 1) m_val++; }
        void decrease() { if (m_val > 0) m_val--; }

        void reset() { m_val = m_init_val; }
        UINT8 getVal() { return m_val; }

        bool isTaken() { return (m_val > (1 << m_wid)/2 - 1); }
};

// 移位寄存器 (N < 128)
class ShiftReg
{
    size_t m_wid;
    UINT128 m_val;

    public:
        ShiftReg(size_t width) : m_wid(width), m_val(0) {}

        bool shiftIn(bool b)
        {
            bool ret = !!(m_val & (1 << (m_wid - 1)));
            m_val <<= 1;
            m_val |= b;
            if(m_wid < 128) m_val &= ((UINT128)1 << (UINT128)m_wid) - (UINT128)1;
            return ret;
        }

        UINT128 getVal() { return m_val; }
};

// Hash functions
inline UINT128 f_xor(UINT128 a, UINT128 b) { return a ^ b; }
// inline UINT128 f_xor1(UINT128 a, UINT128 b) { return a & b; }
inline UINT128 f_xnor(UINT128 a, UINT128 b) { return a ^ b; }


// Base class of all predictors
class BranchPredictor
{
    public:
        BranchPredictor() {}
        virtual ~BranchPredictor() {}
        virtual bool predict(ADDRINT addr) { return false; };
        virtual void update(bool takenActually, bool takenPredicted, ADDRINT addr) {};
};

BranchPredictor* BP;

template<UINT128 (*hash)(UINT128 addr, UINT128 history)>
UINT128 fold(UINT128 a,int alen,UINT128 b, int blen, int flen)
{
    UINT128 ret = truncate(a,flen);
    for (int i = flen; i+flen < alen; i += flen)
    {
        ret = hash(truncate(a >> i,flen),ret);
    }

    for (int i = 0; i+flen < blen; i += flen)
    {
        ret = hash(truncate(b >> i,flen),ret);
    }
    
    // printf("a:%#llx, ",a);
    // printf("alen:%d, ",alen);
    // printf("b:%#llx, ",b);
    // printf("blen:%d, ",blen);
    // printf("fold:%#llx, ",ret);
    // printf("len:%d\n",flen);
    return ret;
}



/* ===================================================================== */
/* BHT-based branch predictor                                            */
/* ===================================================================== */
class BHTPredictor: public BranchPredictor
{
    size_t m_entries_log;
    SaturatingCnt* m_scnt;              // BHT
    allocator<SaturatingCnt> m_alloc;
    
    public:
        // Constructor
        // param:   entry_num_log:  BHT行数的对数
        //          scnt_width:     饱和计数器的位数, 默认值为2
        BHTPredictor(size_t entry_num_log, size_t scnt_width = 2)
        {
            m_entries_log = entry_num_log;

            m_scnt = m_alloc.allocate(1 << entry_num_log);      // Allocate memory for BHT
            for (int i = 0; i < (1 << entry_num_log); i++)
                m_alloc.construct(m_scnt + i, scnt_width);      // Call constructor of SaturatingCnt
        }

        // Destructor
        ~BHTPredictor()
        {
            for (int i = 0; i < (1 << m_entries_log); i++)
                m_alloc.destroy(m_scnt + i);

            m_alloc.deallocate(m_scnt, 1 << m_entries_log);
        }

        BOOL predict(ADDRINT addr)
        {
            //get hash
            int tag = truncate(addr, m_entries_log);

            return m_scnt[tag].isTaken();
        }

        void update(BOOL takenActually, BOOL takenPredicted, ADDRINT addr)
        {
            // TODO: Update BHT according to branch results and prediction

            //get hash
            int tag = truncate(addr, m_entries_log);

            if(takenActually) {
                (m_scnt[tag]).increase();
            } else {
                (m_scnt[tag]).decrease();
            }
        }
};

/* ===================================================================== */
/* Global-history-based branch predictor                                 */
/* ===================================================================== */
template<UINT128 (*hash1)(UINT128 pc, UINT128 ghr), UINT128 (*hash2)(UINT128 pc, UINT128 ghr)>
class GlobalHistoryPredictor: public BranchPredictor
{
    ShiftReg* m_ghr;                   // GHR
    size_t m_ghr_size;
    size_t m_tag_size;
    SaturatingCnt* m_scnt;              // PHT中的分支历史字段
    size_t m_entries_log;                   // PHT行数的对数
    allocator<SaturatingCnt> m_alloc;
    UINT128* m_tags;
    
    public:
        // Constructor
        // param:   ghr_width:      Width of GHR
        //          entry_num_log:  PHT表行数的对数
        //          scnt_width:     饱和计数器的位数, 默认值为2
        GlobalHistoryPredictor(size_t ghr_width, size_t entry_num_log,size_t tag_size ,size_t scnt_width = 2)
        {
            m_ghr = new ShiftReg(ghr_width);
            m_ghr_size = ghr_width;
            m_tag_size = tag_size;
            m_entries_log = entry_num_log;
            m_tags = new UINT128[1 << entry_num_log];
            memset(m_tags,0,sizeof(UINT128)*(1<<entry_num_log));

            m_scnt = m_alloc.allocate(1 << entry_num_log);      // Allocate memory for BHT
            for (int i = 0; i < (1 << entry_num_log); i++)
                m_alloc.construct(m_scnt + i, scnt_width);      // Call constructor of SaturatingCnt
        }

        // Destructor
        ~GlobalHistoryPredictor()
        {
            for (int i = 0; i < (1 << m_entries_log); i++)
                m_alloc.destroy(m_scnt + i);

            m_alloc.deallocate(m_scnt, 1 << m_entries_log);
            delete m_ghr;
            delete[] m_tags;
        }

        // Only for TAGE: return a tag according to the specificed address
        UINT128 get_tag(ADDRINT addr)
        {
            return m_tags[getIdx(addr)];
        }

        // Only for TAGE: return GHR's value
        UINT128 get_ghr()
        {
            return m_ghr->getVal();
        }

        size_t get_ghr_size()
        {
            return m_ghr_size;
        }

        void shift_ghr(bool taken)
        {
            m_ghr->shiftIn(taken);
        }

        void updateTag(ADDRINT addr){
            int idx = getIdx(addr);
            UINT128 new_tag = gen_tag(addr);
            m_tags[idx] = new_tag;
        }

        // Only for TAGE: reset a saturating counter to default value (which is weak taken)
        void reset_ctr(ADDRINT addr)
        {
            //int tag = truncate(hash(addr, m_ghr->getVal()),m_entries_log);
            (m_scnt[getIdx(addr)]).reset();
        }

        bool predict(ADDRINT addr)
        {
            return (m_scnt[getIdx(addr)]).isTaken();
        }

        void update(bool takenActually, bool takenPredicted, ADDRINT addr)
        {
            //update BHT
            // int tag = truncate(hash(addr, m_ghr->getVal()),m_entries_log);
            int idx = getIdx(addr);
            // UINT128 new_tag = gen_tag(addr);
            
            if(takenActually) {
                (m_scnt[idx]).increase();
            } else {
                (m_scnt[idx]).decrease();
            }
            
            // m_tags[idx] = new_tag;

            //update ghr
            m_ghr->shiftIn(takenActually);

            // printf("%d\n",(int)(m_ghr->getVal()));
        }
    
        int getIdx(ADDRINT addr)
        {
            // return truncate(hash(addr,m_ghr->getVal()),m_entries_log);
            return truncate(fold<hash1>(m_ghr->getVal(),m_ghr_size,addr,64,m_entries_log) ,m_entries_log);
        }

        UINT128 gen_tag(ADDRINT addr)
        {
            return truncate(fold<hash2>(m_ghr->getVal(),m_ghr_size,addr,64,m_tag_size) ,m_tag_size);
        }
};

/* ===================================================================== */
/* Tournament predictor: Select output by global/local selection history */
/* ===================================================================== */
class TournamentPredictor: public BranchPredictor
{
    BranchPredictor* m_BPs[2];      // Sub-predictors
    SaturatingCnt* m_gshr;          // Global select-history register

    public:
        TournamentPredictor(BranchPredictor* BP0, BranchPredictor* BP1, size_t gshr_width = 2)
        {
            m_BPs[0] = BP0;
            m_BPs[1] = BP1;
            m_gshr = new SaturatingCnt(gshr_width);
        }

        ~TournamentPredictor()
        {
            delete m_BPs[0];
            delete m_BPs[1];
            delete m_gshr;
        }

        bool predict(ADDRINT addr)
        {
            if (m_gshr->isTaken()) {
                return m_BPs[1]->predict(addr);
            } else {
                return m_BPs[0]->predict(addr);
            }
        };

        void update(bool takenActually, bool takenPredicted, ADDRINT addr)
        {
            //update gshr
            if (m_BPs[1]->predict(addr) == takenActually && m_BPs[0]->predict(addr) != takenActually) {
                m_gshr->increase();
            } else if (m_BPs[1]->predict(addr) != takenActually && m_BPs[0]->predict(addr) == takenActually) {
                m_gshr->decrease();
            }

            //update sub BP
            m_BPs[0]->update(takenActually, takenPredicted, addr);
            m_BPs[1]->update(takenActually, takenPredicted, addr);
        };
};

/* ===================================================================== */
/* TArget GEometric history length Predictor                             */
/* ===================================================================== */
template<UINT128 (*hash1)(UINT128 pc, UINT128 ghr), UINT128 (*hash2)(UINT128 pc, UINT128 ghr)>
class TAGEPredictor: public BranchPredictor
{
    const size_t m_tnum;            // 子预测器个数 (T[0 : m_tnum - 1])
    const size_t m_entries_log;     // 子预测器T[1 : m_tnum - 1]的PHT行数的对数
    BranchPredictor** m_T;          // 子预测器指针数组
    bool* m_T_pred;                 // 用于存储各子预测的预测值
    UINT8** m_useful;               // usefulness matrix
    int provider_indx;              // Provider's index of m_T
    int altpred_indx;               // Alternate provider's index of m_T
    size_t m_tag_size;

    const size_t m_rst_period;      // Reset period of usefulness
    size_t m_rst_cnt;               // Reset counter

    public:
        // Constructor
        // param:   tnum:               The number of sub-predictors
        //          T0_entry_num_log:   子预测器T0的BHT行数的对数
        //          T1ghr_len:          子预测器T1的GHR位宽
        //          alpha:              各子预测器T[1 : m_tnum - 1]的GHR几何倍数关系
        //          Tn_entry_num_log:   各子预测器T[1 : m_tnum - 1]的PHT行数的对数
        //          scnt_width:         Width of saturating counter (3 by default)
        //          rst_period:         Reset period of usefulness
        TAGEPredictor(size_t tnum, size_t T0_entry_num_log, size_t T1ghr_len, float alpha, size_t Tn_entry_num_log, size_t tag_size,size_t scnt_width = 3, size_t rst_period = 256*1024)
        : m_tnum(tnum), m_entries_log(Tn_entry_num_log),m_tag_size(tag_size), m_rst_period(rst_period),m_rst_cnt(0)
        {
            m_T = new BranchPredictor* [m_tnum];
            m_T_pred = new bool [m_tnum];
            m_useful = new UINT8* [m_tnum];

            m_T[0] = new BHTPredictor(T0_entry_num_log);

            size_t ghr_size = T1ghr_len;
            for (size_t i = 1; i < m_tnum; i++)
            {
                m_T[i] = new GlobalHistoryPredictor<hash1,hash2>(ghr_size, m_entries_log, m_tag_size,scnt_width);
                ghr_size = (size_t)(ghr_size * alpha);
                m_useful[i] = new UINT8 [1 << m_entries_log];
                memset(m_useful[i], 0, sizeof(UINT8)*(1 << m_entries_log));
                //memset(m_tags[i-1], 0, sizeof(UINT128)*(1 << m_entries_log));
            }
        }

        ~TAGEPredictor()
        {
            for (size_t i = 0; i < m_tnum; i++) delete m_T[i];
            for (size_t i = 0; i < m_tnum; i++) delete[] m_useful[i];

            delete[] m_T;
            delete[] m_T_pred;
            delete[] m_useful;
        }

        bool predict(ADDRINT addr)
        {
            provider_indx = 0;
            altpred_indx = 0;
            
            // from longest to shortest
            for (int i = m_tnum - 1; i > 0; i--) {
                UINT128 tag1 = ((GlobalHistoryPredictor<hash1,hash2>*)m_T[i])->get_tag(addr);
                UINT128 tag2 = ((GlobalHistoryPredictor<hash1,hash2>*)m_T[i])->gen_tag(addr);
                //UINT128 tag2 = hash1(addr,((GlobalHistoryPredictor<hash1>*)m_T[i])->get_ghr());
                if (tag1 == tag2) {
                    if (provider_indx == 0) provider_indx = i;
                    else {
                        altpred_indx = i;
                        break;
                    }
                }
            }
            m_T_pred[provider_indx] = m_T[provider_indx]->predict(addr);
            m_T_pred[altpred_indx] = m_T[altpred_indx]->predict(addr);
            
            
            return m_T_pred[provider_indx];
        }

        void update(bool takenActually, bool takenPredicted, ADDRINT addr)
        {
            // if(provider_indx != 0) 
            // {
            //     GlobalHistoryPredictor<hash1,hash2>* BPQ = (GlobalHistoryPredictor<hash1,hash2>*)m_T[provider_indx];
            //     // UINT128 GHP = BPQ->get_ghr();
            //     // printf("%llu\n",BPQ->get_ghr());
            //     // UINT128 GHP = ((UINT128)1 << 127) - (UINT128)1;
                
            //     printf("Prov:%d, Alt:%d,prid %d, take %d,",provider_indx,altpred_indx,takenPredicted,takenActually);
            //     printf(" ghr:%llu\n",BPQ->get_ghr());
            // }


            // printf("Take %d\n",takenActually);
            // TODO: Update usefulness
            if(m_T_pred[provider_indx] != m_T_pred[altpred_indx]){
                int idx = ((GlobalHistoryPredictor<hash1,hash2>*)m_T[provider_indx])->getIdx(addr);

                if (takenPredicted == takenActually) {
                    if(m_useful[provider_indx][idx] < 3) m_useful[provider_indx][idx]++;
                } else {
                    if(m_useful[provider_indx][idx] > 0) m_useful[provider_indx][idx]--;
                }
            }

            

            

            // TODO: Update provider itself
            // printf("Update %d\n",provider_indx);
            m_T[provider_indx]-> update(takenActually, takenPredicted, addr);

            for (int i = 1; i < m_tnum; i++) {
                if(i == provider_indx) continue;
                ((GlobalHistoryPredictor<hash1,hash2>*)m_T[i]) -> shift_ghr(takenActually);
            }


            // TODO: Entry replacement
            bool allocated = false;
            if (takenPredicted != takenActually) {
                for (size_t i = provider_indx+1; i<m_tnum; i++) {
                    int idx = ((GlobalHistoryPredictor<hash1,hash2>*)m_T[i])->getIdx(addr);
                    if(m_useful[i][idx] == 0){
                        ((GlobalHistoryPredictor<hash1,hash2>*)m_T[i])->updateTag(addr);
                        ((GlobalHistoryPredictor<hash1,hash2>*)m_T[i])->reset_ctr(addr);
                        allocated = true;
                        break;
                    }
                }
                if (!allocated) {
                    for (size_t i = provider_indx+1; i<m_tnum; i++) {
                        int getIdx = ((GlobalHistoryPredictor<hash1,hash2>*)m_T[i])->getIdx(addr);
                        if(m_useful[i][getIdx] != 0) m_useful[i][getIdx]--;
                    }
                }
            }

            

            // TODO: Reset usefulness periodically
            if(m_rst_cnt >= m_rst_period){
                m_rst_cnt = 0;
                for (size_t i = 1; i < m_tnum; i++){
                    memset(m_useful[i], 0, sizeof(UINT8)*(1 << m_entries_log));
                }
            } else {
                m_rst_cnt++;
            }
        }

        
};



// This function is called every time a control-flow instruction is encountered
void predictBranch(ADDRINT pc, BOOL direction)
{
    BOOL prediction = BP->predict(pc);
    BP->update(direction, prediction, pc);
    if (prediction)
    {
        if (direction)
            takenCorrect++;
        else
            takenIncorrect++;
    }
    else
    {
        if (direction)
            notTakenIncorrect++;
        else
            notTakenCorrect++;
    }
}

// Pin calls this function every time a new instruction is encountered
void Instruction(INS ins, void * v)
{
    if (INS_IsControlFlow(ins) && INS_HasFallThrough(ins))
    {
        // Insert a call to the branch target
        INS_InsertCall(ins, IPOINT_TAKEN_BRANCH, (AFUNPTR)predictBranch,
                        IARG_INST_PTR, IARG_BOOL, TRUE, IARG_END);

        // Insert a call to the next instruction of a branch
        INS_InsertCall(ins, IPOINT_AFTER, (AFUNPTR)predictBranch,
                        IARG_INST_PTR, IARG_BOOL, FALSE, IARG_END);
    }
}

// This knob sets the output file name
KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "o", "brchPredict.txt", "specify the output file name");

// This function is called when the application exits
VOID Fini(int, VOID * v)
{
	double precision = 100 * double(takenCorrect + notTakenCorrect) / (takenCorrect + notTakenCorrect + takenIncorrect + notTakenIncorrect);
    
    cout << "takenCorrect: " << takenCorrect << endl
    	<< "takenIncorrect: " << takenIncorrect << endl
    	<< "notTakenCorrect: " << notTakenCorrect << endl
    	<< "nnotTakenIncorrect: " << notTakenIncorrect << endl
    	<< "Precision: " << precision << endl;
    
    OutFile.setf(ios::showbase);
    OutFile << "takenCorrect: " << takenCorrect << endl
    	<< "takenIncorrect: " << takenIncorrect << endl
    	<< "notTakenCorrect: " << notTakenCorrect << endl
    	<< "nnotTakenIncorrect: " << notTakenIncorrect << endl
    	<< "Precision: " << precision << endl;
    
    OutFile.close();
    delete BP;
}

/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */

INT32 Usage()
{
    cerr << "This tool counts the number of dynamic instructions executed" << endl;
    cerr << endl << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */
/*   argc, argv are the entire command line: pin -t <toolname> -- ...    */
/* ===================================================================== */

int main(int argc, char * argv[])
{
    // TODO: New your Predictor below.
    // BP = new BHTPredictor(14);
    // BP = new GlobalHistoryPredictor<f_xor,f_xnor>(22,11,9);
    // BP = new TournamentPredictor(new GlobalHistoryPredictor<f_xor>(13,13),new GlobalHistoryPredictor<f_xnor>(13,13));
    BP = new TAGEPredictor<f_xor, f_xnor>(8, 14, 2, 2, 11,12);
    
    // Initialize pin
    if (PIN_Init(argc, argv)) return Usage();
    
    OutFile.open(KnobOutputFile.Value().c_str());

    // Register Instruction to be called to instrument instructions
    INS_AddInstrumentFunction(Instruction, 0);

    // Register Fini to be called when the application exits
    PIN_AddFiniFunction(Fini, 0);

    // Start the program, never returns
    PIN_StartProgram();

    return 0;
}
