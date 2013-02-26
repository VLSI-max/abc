/**CFile****************************************************************

  FileName    [giaSweeper.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Scalable AIG package.]

  Synopsis    [Incremental SAT sweeper.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: giaSweeper.c,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#include "gia.h"
#include "sat/bsat/satSolver.h"

ABC_NAMESPACE_IMPL_START

/*

SAT sweeping/equivalence checking requires the following steps:
- Creating probes
  These APIs should be called for all internal points in the logic, which may be used as
      - nodes representing conditions to be used as constraints
      - nodes representing functions to be equivalence checked
      - nodes representing functions needed by the user at the end of SAT sweeping
  Creating new probe using  Gia_SweeperProbeCreate(): int Gia_SweeperProbeCreate( Gia_Man_t * p, int iLit );
  Find existing probe using Gia_SweeperProbeFind():   int Gia_SweeperProbeFind( Gia_Man_t * p, int iLit );
      If probe with this literal (iLit) exists, this procedure increments its reference counter and returns it.
      If probe with this literal does not exist, it creates new probe and sets is reference counter to 1.
  Dereference probe using Gia_SweeperProbeDeref():    void Gia_SweeperProbeDeref( Gia_Man_t * p, int ProbeId );
      Dereferences probe with the given ID. If ref counter become 0, recycles the logic cone of the given probe.
      Recycling of probe IDs can be added later.
  Comments:
      - a probe is identified by its 0-based ID, which is returned by above procedures
      - GIA literal of the probe is returned by       int Gia_SweeperProbeLit( Gia_Man_t * p, int ProbeId )
- Adding/removing conditions on the current path by calling Gia_SweeperCondPush() and Gia_SweeperCondPop()
      extern void Gia_SweeperCondPush( Gia_Man_t * p, int ProbeId );
      extern void Gia_SweeperCondPop( Gia_Man_t * p );
- Performing equivalence checking by calling     int Gia_SweeperCheckEquiv( Gia_Man_t * pGia, int Probe1, int Probe2 )
      (resource limits, such as the number of conflicts, will be controllable by dedicated GIA APIs)
- The resulting AIG to be returned to the user by calling Gia_SweeperExtractUserLogic()
      Gia_Man_t * Gia_SweeperExtractUserLogic( Gia_Man_t * p, Vec_Int_t * vProbeIds, Vec_Ptr_t * vOutNames )

*/

////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

typedef struct Swp_Man_t_ Swp_Man_t;
struct Swp_Man_t_
{
    Gia_Man_t *    pGia;        // GIA manager under construction
    int            nConfMax;    // conflict limit in seconds
    int            nTimeOut;    // runtime limit in seconds
    Vec_Int_t *    vProbes;     // probes
    Vec_Int_t *    vProbRefs;   // probe reference counters
    Vec_Int_t *    vLit2Prob;   // mapping of literal into its probe
//    Vec_Int_t *    vFreeProb;   // recycled probe IDs
    Vec_Int_t *    vCondProbes; // conditions as probes
    Vec_Int_t *    vCondLits;   // conditions as literals
    Vec_Int_t *    vCondAssump; // conditions as SAT solver literals
    // equivalence checking
    sat_solver *   pSat;        // SAT solver 
    Vec_Int_t *    vId2Lit;     // mapping of Obj IDs into SAT literal
    Vec_Int_t *    vFront;      // temporary frontier
    Vec_Int_t *    vFanins;     // temporary fanins
    Vec_Int_t *    vCexSwp;     // sweeper counter-example
    Vec_Int_t *    vCexUser;    // user-visible counter-example
    int            nSatVars;    // counter of SAT variables
    // statistics
    int            nSatCalls;
    int            nSatCallsSat;
    int            nSatCallsUnsat;
    int            nSatFails;
    int            nSatProofs;
    clock_t        timeSat;
    clock_t        timeSatSat;
    clock_t        timeSatUnsat;
    clock_t        timeSatUndec;
};

static inline int  Swp_ManObj2Lit( Swp_Man_t * p, int Id )              { return Vec_IntGetEntry( p->vId2Lit, Id );              }
static inline int  Swp_ManLit2Lit( Swp_Man_t * p, int Lit )             { return Abc_Lit2LitL( Vec_IntArray(p->vId2Lit), Lit );  }
static inline void Swp_ManSetObj2Lit( Swp_Man_t * p, int Id, int Lit )  { Vec_IntSetEntry( p->vId2Lit, Id, Lit );                }

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////
    
/**Function*************************************************************

  Synopsis    [Creating/deleting the manager.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline Swp_Man_t * Swp_ManStart( Gia_Man_t * pGia )
{
    Swp_Man_t * p;
    int Lit;
    pGia->pData = p = ABC_CALLOC( Swp_Man_t, 1 );
    p->pGia         = pGia;
    p->nConfMax     = 1000;
    p->vProbes      = Vec_IntAlloc( 100 );
    p->vProbRefs    = Vec_IntAlloc( 100 );
    p->vLit2Prob    = Vec_IntStartFull( 10000 );
    p->vCondProbes  = Vec_IntAlloc( 100 );
    p->vCondLits    = Vec_IntAlloc( 100 );
    p->vCondAssump  = Vec_IntAlloc( 100 );
    p->vId2Lit      = Vec_IntAlloc( 10000 );
    p->vFront       = Vec_IntAlloc( 100 );
    p->vFanins      = Vec_IntAlloc( 100 );
    p->vCexSwp      = Vec_IntAlloc( 100 );
    p->pSat         = sat_solver_new();
    p->nSatVars     = 1;
    sat_solver_setnvars( p->pSat, 1000 );
    Swp_ManSetObj2Lit( p, 0, (Lit = Abc_Var2Lit(p->nSatVars++, 0)) );
    Lit = Abc_LitNot(Lit);
    sat_solver_addclause( p->pSat, &Lit, &Lit + 1 );
    return p;
}
static inline void Swp_ManStop( Gia_Man_t * pGia )
{
    Swp_Man_t * p = (Swp_Man_t *)pGia->pData;
    sat_solver_delete( p->pSat );
    Vec_IntFree( p->vFanins );
    Vec_IntFree( p->vCexSwp );
    Vec_IntFree( p->vId2Lit );
    Vec_IntFree( p->vFront );
    Vec_IntFree( p->vProbes );
    Vec_IntFree( p->vProbRefs );
    Vec_IntFree( p->vLit2Prob );
    Vec_IntFree( p->vCondProbes );
    Vec_IntFree( p->vCondLits );
    Vec_IntFree( p->vCondAssump );
    ABC_FREE( p );
    pGia->pData = NULL;
}
Gia_Man_t * Gia_SweeperStart()
{
    Gia_Man_t * pGia;
    pGia = Gia_ManStart( 10000 );
    Gia_ManHashStart( pGia );
    Swp_ManStart( pGia );
    pGia->fSweeper = 1;
    return pGia;
}
void Gia_SweeperStop( Gia_Man_t * pGia )
{
    pGia->fSweeper = 0;
    Swp_ManStop( pGia );
    Gia_ManHashStop( pGia );
    Gia_ManStop( pGia );
}

/**Function*************************************************************

  Synopsis    [Setting resource limits.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Gia_SweeperSetConflictLimit( Gia_Man_t * p, int nConfMax )
{
    Swp_Man_t * pSwp = (Swp_Man_t *)p->pData;
    pSwp->nConfMax = nConfMax;
}

void Gia_SweeperSetRuntimeLimit( Gia_Man_t * p, int nSeconds )
{
    Swp_Man_t * pSwp = (Swp_Man_t *)p->pData;
    pSwp->nTimeOut = nSeconds;
    if ( nSeconds )
        sat_solver_set_runtime_limit( pSwp->pSat, nSeconds * CLOCKS_PER_SEC + clock() );
}
Vec_Int_t * Gia_SweeperGetCex( Gia_Man_t * p )
{
    Swp_Man_t * pSwp = (Swp_Man_t *)p->pData;
    return pSwp->vCexUser;
}
    
/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
// create new probe
int Gia_SweeperProbeCreate( Gia_Man_t * p, int iLit )
{
    Swp_Man_t * pSwp = (Swp_Man_t *)p->pData;
    int ProbeId = Vec_IntSize(pSwp->vProbes);
    Vec_IntPush( pSwp->vProbes, iLit );
    Vec_IntPush( pSwp->vProbRefs, 1 );
    Vec_IntSetEntryFull( pSwp->vLit2Prob, iLit, ProbeId ); // consider hash table in the future
//printf( "Creating probe %d with literal %d.\n", ProbeId, iLit );
    return ProbeId;
}
// if probe with this literal (iLit) exists, this procedure increments its reference counter and returns it.
// if probe with this literal does not exist, it creates new probe and sets is reference counter to 1.
int Gia_SweeperProbeFind( Gia_Man_t * p, int iLit )
{
    Swp_Man_t * pSwp = (Swp_Man_t *)p->pData;
    if ( iLit < Vec_IntSize(pSwp->vLit2Prob) && Vec_IntEntry(pSwp->vLit2Prob, iLit) >= 0 )
        return Vec_IntEntry(pSwp->vLit2Prob, iLit);
    return Gia_SweeperProbeCreate( p, iLit );
}
// dereferences the probe
void Gia_SweeperProbeDeref( Gia_Man_t * p, int ProbeId )
{
    Swp_Man_t * pSwp = (Swp_Man_t *)p->pData;
    assert( Vec_IntEntry(pSwp->vProbRefs, ProbeId) > 0 );
    if ( Vec_IntAddToEntry( pSwp->vProbRefs, ProbeId, -1 ) == 0 )
    {
        int iLit = Gia_SweeperProbeLit( p, ProbeId );
        Vec_IntWriteEntry( pSwp->vLit2Prob, iLit, -1 );
        Vec_IntWriteEntry( pSwp->vProbes, ProbeId, 0 );
        // TODO: recycle probe ID
//printf( "Deleteing probe %d with literal %d.\n", ProbeId, iLit );
    }
}
// returns literal associated with the probe
int Gia_SweeperProbeLit( Gia_Man_t * p, int ProbeId )
{
    Swp_Man_t * pSwp = (Swp_Man_t *)p->pData;
    return Vec_IntEntry( pSwp->vProbes, ProbeId );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Gia_SweeperCondPush( Gia_Man_t * p, int ProbeId )
{
    Swp_Man_t * pSwp = (Swp_Man_t *)p->pData;
    Vec_IntPush( pSwp->vCondProbes, ProbeId );
    Vec_IntPush( pSwp->vCondLits, Gia_SweeperProbeLit(p, ProbeId) );
}
int Gia_SweeperCondPop( Gia_Man_t * p )
{
    Swp_Man_t * pSwp = (Swp_Man_t *)p->pData;
    int ProbId = Vec_IntPop( pSwp->vCondProbes );
    Vec_IntPop( pSwp->vCondLits );
    return ProbId;
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline Vec_Ptr_t * Vec_PtrDupStr( Vec_Ptr_t * pVec )
{
    char * pName;
    int i;
    Vec_Ptr_t * p = Vec_PtrDup( pVec );
    Vec_PtrForEachEntry( char *, p, pName, i )
        Vec_PtrWriteEntry( p, i, Abc_UtilStrsav(pName) );
    return p;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static void Gia_ManExtract_rec( Gia_Man_t * p, Gia_Obj_t * pObj, Vec_Int_t * vObjIds )
{
    if ( !Gia_ObjIsAnd(pObj) )
        return;
    if ( Gia_ObjIsTravIdCurrent(p, pObj) )
        return;
    Gia_ObjSetTravIdCurrent(p, pObj);
    Gia_ManExtract_rec( p, Gia_ObjFanin0(pObj), vObjIds );
    Gia_ManExtract_rec( p, Gia_ObjFanin1(pObj), vObjIds );
    Vec_IntPush( vObjIds, Gia_ObjId(p, pObj) );
}
Gia_Man_t * Gia_SweeperExtractUserLogic( Gia_Man_t * p, Vec_Int_t * vProbeIds, Vec_Ptr_t * vOutNames )
{
    Gia_Man_t * pNew, * pTemp;
    Gia_Obj_t * pObj;
    Vec_Int_t * vObjIds;
    Vec_Int_t * vValues;
    int i, ProbeId;
    assert( Vec_IntSize(vProbeIds) == Vec_PtrSize(vOutNames) );
    // save values
    vValues = Vec_IntAlloc( Gia_ManObjNum(p) );
    Gia_ManForEachObj( p, pObj, i )
    {
        Vec_IntPush( vValues, pObj->Value );
        pObj->Value = ~0;
    }
    // create new
    Gia_ManIncrementTravId( p );
    vObjIds = Vec_IntAlloc( 1000 );
    Vec_IntForEachEntry( vProbeIds, ProbeId, i )
    {
        pObj = Gia_Lit2Obj( p, Gia_SweeperProbeLit(p, ProbeId) );
        Gia_ManExtract_rec( p, Gia_Regular(pObj), vObjIds );
    }
    // create new manager
    pNew = Gia_ManStart( Gia_ManObjNum(p) );
    pNew->pName = Abc_UtilStrsav( p->pName );
    pNew->pSpec = Abc_UtilStrsav( p->pSpec );
    Gia_ManConst0(p)->Value = 0;
    Gia_ManForEachPi( p, pObj, i )
        pObj->Value = Gia_ManAppendCi(pNew);
    // create internal nodes
    Gia_ManHashStart( pNew );
    Gia_ManForEachObjVec( vObjIds, p, pObj, i )
        pObj->Value = Gia_ManHashAnd( pNew, Gia_ObjFanin0Copy(pObj), Gia_ObjFanin1Copy(pObj) );
    Gia_ManHashStop( pNew );
    Vec_IntFree( vObjIds );
    // create outputs
    Vec_IntForEachEntry( vProbeIds, ProbeId, i )
    {
        pObj = Gia_Lit2Obj( p, Gia_SweeperProbeLit(p, ProbeId) );
        Gia_ManAppendCo( pNew, Gia_Regular(pObj)->Value ^ Gia_IsComplement(pObj) );
    }
    // duplicated if needed
    if ( Gia_ManHasDangling(pNew) )
    {
        pNew = Gia_ManCleanup( pTemp = pNew );
        Gia_ManStop( pTemp );
    }
    // copy names if present
    if ( p->vNamesIn )
        pNew->vNamesIn = Vec_PtrDupStr( p->vNamesIn );
    if ( vOutNames )
        pNew->vNamesOut = Vec_PtrDupStr( vOutNames );
//Gia_ManPrintStats( pNew, 0, 0, 0 );
    // reset values
    Gia_ManForEachObj( p, pObj, i )
        pObj->Value = Vec_IntEntry( vValues, i );
    Vec_IntFree( vValues );
    return pNew;
}


/**Function*************************************************************

  Synopsis    [Addes clauses to the solver.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static void Gia_ManAddClausesMux( Swp_Man_t * p, Gia_Obj_t * pNode )
{
    Gia_Obj_t * pNodeI, * pNodeT, * pNodeE;
    int pLits[4], LitF, LitI, LitT, LitE, RetValue;
    assert( !Gia_IsComplement( pNode ) );
    assert( Gia_ObjIsMuxType( pNode ) );
    // get nodes (I = if, T = then, E = else)
    pNodeI = Gia_ObjRecognizeMux( pNode, &pNodeT, &pNodeE );
    // get the Litiable numbers
    LitF = Swp_ManLit2Lit( p, Gia_Obj2Lit(p->pGia,pNode) );
    LitI = Swp_ManLit2Lit( p, Gia_Obj2Lit(p->pGia,pNodeI) );
    LitT = Swp_ManLit2Lit( p, Gia_Obj2Lit(p->pGia,pNodeT) );
    LitE = Swp_ManLit2Lit( p, Gia_Obj2Lit(p->pGia,pNodeE) );

    // f = ITE(i, t, e)

    // i' + t' + f
    // i' + t  + f'
    // i  + e' + f
    // i  + e  + f'

    // create four clauses
    pLits[0] = Abc_LitNotCond(LitI, 1);
    pLits[1] = Abc_LitNotCond(LitT, 1);
    pLits[2] = Abc_LitNotCond(LitF, 0);
    RetValue = sat_solver_addclause( p->pSat, pLits, pLits + 3 );
    assert( RetValue );
    pLits[0] = Abc_LitNotCond(LitI, 1);
    pLits[1] = Abc_LitNotCond(LitT, 0);
    pLits[2] = Abc_LitNotCond(LitF, 1);
    RetValue = sat_solver_addclause( p->pSat, pLits, pLits + 3 );
    assert( RetValue );
    pLits[0] = Abc_LitNotCond(LitI, 0);
    pLits[1] = Abc_LitNotCond(LitE, 1);
    pLits[2] = Abc_LitNotCond(LitF, 0);
    RetValue = sat_solver_addclause( p->pSat, pLits, pLits + 3 );
    assert( RetValue );
    pLits[0] = Abc_LitNotCond(LitI, 0);
    pLits[1] = Abc_LitNotCond(LitE, 0);
    pLits[2] = Abc_LitNotCond(LitF, 1);
    RetValue = sat_solver_addclause( p->pSat, pLits, pLits + 3 );
    assert( RetValue );

    // two additional clauses
    // t' & e' -> f'
    // t  & e  -> f

    // t  + e   + f'
    // t' + e'  + f

    if ( LitT == LitE )
    {
//        assert( fCompT == !fCompE );
        return;
    }

    pLits[0] = Abc_LitNotCond(LitT, 0);
    pLits[1] = Abc_LitNotCond(LitE, 0);
    pLits[2] = Abc_LitNotCond(LitF, 1);
    RetValue = sat_solver_addclause( p->pSat, pLits, pLits + 3 );
    assert( RetValue );
    pLits[0] = Abc_LitNotCond(LitT, 1);
    pLits[1] = Abc_LitNotCond(LitE, 1);
    pLits[2] = Abc_LitNotCond(LitF, 0);
    RetValue = sat_solver_addclause( p->pSat, pLits, pLits + 3 );
    assert( RetValue );
}

/**Function*************************************************************

  Synopsis    [Addes clauses to the solver.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
static void Gia_ManAddClausesSuper( Swp_Man_t * p, Gia_Obj_t * pNode, Vec_Int_t * vSuper )
{
    int i, RetValue, Lit, LitNode, pLits[2];
    assert( !Gia_IsComplement(pNode) );
    assert( Gia_ObjIsAnd( pNode ) );
    // suppose AND-gate is A & B = C
    // add !A => !C   or   A + !C
    // add !B => !C   or   B + !C
    LitNode = Swp_ManLit2Lit( p, Gia_Obj2Lit(p->pGia,pNode) );
    Vec_IntForEachEntry( vSuper, Lit, i )
    {
        pLits[0] = Swp_ManLit2Lit( p, Lit );
        pLits[1] = Abc_LitNot( LitNode );
        RetValue = sat_solver_addclause( p->pSat, pLits, pLits + 2 );
        assert( RetValue );
        // update literals
        Vec_IntWriteEntry( vSuper, i, Abc_LitNot(pLits[0]) );
    }
    // add A & B => C   or   !A + !B + C
    Vec_IntPush( vSuper, LitNode );
    RetValue = sat_solver_addclause( p->pSat, Vec_IntArray(vSuper), Vec_IntArray(vSuper) + Vec_IntSize(vSuper) );
    assert( RetValue );
    (void) RetValue;
}


/**Function*************************************************************

  Synopsis    [Collects the supergate.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
static void Gia_ManCollectSuper_rec( Gia_Man_t * p, Gia_Obj_t * pObj, Vec_Int_t * vSuper )
{
    // stop at complements, shared, PIs, and MUXes
    if ( Gia_IsComplement(pObj) || pObj->fMark1 || Gia_ObjIsCi(pObj) || Gia_ObjIsMuxType(pObj) )
    {
        Vec_IntPushUnique( vSuper, Gia_Obj2Lit(p, pObj) );
        return;
    }
    Gia_ManCollectSuper_rec( p, Gia_ObjChild0(pObj), vSuper );
    Gia_ManCollectSuper_rec( p, Gia_ObjChild1(pObj), vSuper );
}
static void Gia_ManCollectSuper( Gia_Man_t * p, Gia_Obj_t * pObj, Vec_Int_t * vSuper )
{
    assert( !Gia_IsComplement(pObj) );
    assert( Gia_ObjIsAnd(pObj) );
    Vec_IntClear( vSuper );
    Gia_ManCollectSuper_rec( p, Gia_ObjChild0(pObj), vSuper );
    Gia_ManCollectSuper_rec( p, Gia_ObjChild1(pObj), vSuper );
}

/**Function*************************************************************

  Synopsis    [Updates the solver clause database.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
static void Gia_ManObjAddToFrontier( Swp_Man_t * p, int Id, Vec_Int_t * vFront )
{
    Gia_Obj_t * pObj;
    if ( Id == 0 || Swp_ManObj2Lit(p, Id) )
        return;
    pObj = Gia_ManObj( p->pGia, Id );
    Swp_ManSetObj2Lit( p, Id, Abc_Var2Lit(p->nSatVars++, pObj->fPhase) );
    sat_solver_setnvars( p->pSat, p->nSatVars + 100 );
    if ( Gia_ObjIsAnd(pObj) )
        Vec_IntPush( vFront, Id );
}
static void Gia_ManCnfNodeAddToSolver( Swp_Man_t * p, int NodeId )
{
    Gia_Obj_t * pNode;
    int i, k, Id, Lit;
    // quit if CNF is ready
    if ( NodeId == 0 || Swp_ManObj2Lit(p, NodeId) )
        return;
    // start the frontier
    Vec_IntClear( p->vFront );
    Gia_ManObjAddToFrontier( p, NodeId, p->vFront );
    // explore nodes in the frontier
    Gia_ManForEachObjVec( p->vFront, p->pGia, pNode, i )
    {
        // create the supergate
        assert( Swp_ManObj2Lit(p, Gia_ObjId(p->pGia, pNode)) );
        if ( Gia_ObjIsMuxType(pNode) )
        {
            Vec_IntClear( p->vFanins );
            Vec_IntPushUnique( p->vFanins, Gia_ObjFaninId0p( p->pGia, Gia_ObjFanin0(pNode) ) );
            Vec_IntPushUnique( p->vFanins, Gia_ObjFaninId0p( p->pGia, Gia_ObjFanin1(pNode) ) );
            Vec_IntPushUnique( p->vFanins, Gia_ObjFaninId1p( p->pGia, Gia_ObjFanin0(pNode) ) );
            Vec_IntPushUnique( p->vFanins, Gia_ObjFaninId1p( p->pGia, Gia_ObjFanin1(pNode) ) );
            Vec_IntForEachEntry( p->vFanins, Id, k )
                Gia_ManObjAddToFrontier( p, Id, p->vFront );
            Gia_ManAddClausesMux( p, pNode );
        }
        else
        {
            Gia_ManCollectSuper( p->pGia, pNode, p->vFanins );
            Vec_IntForEachEntry( p->vFanins, Lit, k )
                Gia_ManObjAddToFrontier( p, Abc_Lit2Var(Lit), p->vFront );
            Gia_ManAddClausesSuper( p, pNode, p->vFanins );
        }
        assert( Vec_IntSize(p->vFanins) > 1 );
    }
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static Vec_Int_t * Gia_ManGetCex( Gia_Man_t * pGia, Vec_Int_t * vId2Lit, sat_solver * pSat, Vec_Int_t * vCex )
{
    Gia_Obj_t * pObj;
    int i, LitSat, Value;
    Vec_IntClear( vCex );
    Gia_ManForEachPi( pGia, pObj, i )
    {
        LitSat = Vec_IntEntry( vId2Lit, Gia_ObjId(pGia, pObj) );
        assert( LitSat > 0 );
        Value = sat_solver_var_value(pSat, Abc_Lit2Var(LitSat)) ^ Abc_LitIsCompl(LitSat);
        Vec_IntPush( vCex, Value );
    }
    return vCex;
}

/**Function*************************************************************

  Synopsis    [Runs equivalence test for probes.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Gia_SweeperCheckEquiv( Gia_Man_t * pGia, int Probe1, int Probe2 )
{
    Swp_Man_t * p = (Swp_Man_t *)pGia->pData;
    int iLitOld, iLitNew, pLitsSat[2], RetValue, RetValue1, iLitAig, i;
    clock_t clk;
    p->nSatCalls++;
    assert( p->pSat != NULL );
    p->vCexUser = NULL;

    // get the literals
    iLitOld = Gia_SweeperProbeLit( pGia, Probe1 );
    iLitNew = Gia_SweeperProbeLit( pGia, Probe2 );
    // if the literals are identical, the probes are equivalent
    if ( iLitOld == iLitNew )
        return 1;
    // if the literals are opposites, the probes are not equivalent
    if ( Abc_LitRegular(iLitOld) == Abc_LitRegular(iLitNew) )
    {
        Vec_IntFill( p->vCexSwp, Gia_ManPiNum(pGia), 0 );
        p->vCexUser = p->vCexSwp;
        return 0;
    }
    // order the literals
    if ( iLitOld < iLitNew )
        ABC_SWAP( int, iLitOld, iLitNew );
    assert( iLitOld > iLitNew );

    // if the nodes do not have SAT variables, allocate them
    Vec_IntClear( p->vCondAssump );
    Vec_IntForEachEntry( p->vCondLits, iLitAig, i )
    {
        Gia_ManCnfNodeAddToSolver( p, Abc_Lit2Var(iLitAig) );
        Vec_IntPush( p->vCondAssump, Swp_ManLit2Lit(p, iLitAig) );
    }
    Gia_ManCnfNodeAddToSolver( p, Abc_Lit2Var(iLitOld) );
    Gia_ManCnfNodeAddToSolver( p, Abc_Lit2Var(iLitNew) );
    sat_solver_compress( p->pSat );

    // set the SAT literals
    pLitsSat[0] = Swp_ManLit2Lit( p, iLitOld );
    pLitsSat[1] = Swp_ManLit2Lit( p, iLitNew );

    // solve under assumptions
    // A = 1; B = 0     OR     A = 1; B = 1 
    Vec_IntPush( p->vCondAssump, pLitsSat[0] );
    Vec_IntPush( p->vCondAssump, Abc_LitNot(pLitsSat[1]) );

clk = clock();
    RetValue1 = sat_solver_solve( p->pSat, Vec_IntArray(p->vCondAssump), Vec_IntArray(p->vCondAssump) + Vec_IntSize(p->vCondAssump), 
        (ABC_INT64_T)p->nConfMax, (ABC_INT64_T)0, (ABC_INT64_T)0, (ABC_INT64_T)0 );
    Vec_IntShrink( p->vCondAssump, Vec_IntSize(p->vCondAssump) - 2 );
p->timeSat += clock() - clk;
    if ( RetValue1 == l_False )
    {
        pLitsSat[0] = Abc_LitNot( pLitsSat[0] );
        pLitsSat[1] = Abc_LitNot( pLitsSat[1] );
        RetValue = sat_solver_addclause( p->pSat, pLitsSat, pLitsSat + 2 );
        assert( RetValue );
        pLitsSat[0] = Abc_LitNot( pLitsSat[0] );
        pLitsSat[1] = Abc_LitNot( pLitsSat[1] );
p->timeSatUnsat += clock() - clk;
        p->nSatCallsUnsat++;
    }
    else if ( RetValue1 == l_True )
    {
        p->vCexUser = Gia_ManGetCex( p->pGia, p->vId2Lit, p->pSat, p->vCexSwp );
p->timeSatSat += clock() - clk;
        p->nSatCallsSat++;
        return 0;
    }
    else // if ( RetValue1 == l_Undef )
    {
p->timeSatUndec += clock() - clk;
        p->nSatFails++;
        return -1;
    }

    // if the old node was constant 0, we already know the answer
    if ( Gia_ManIsConstLit(iLitNew) )
    {
        p->nSatProofs++;
        return 1;
    }

    // solve under assumptions
    // A = 0; B = 1     OR     A = 0; B = 0 
    Vec_IntPush( p->vCondAssump, Abc_LitNot(pLitsSat[0]) );
    Vec_IntPush( p->vCondAssump, pLitsSat[1] );

clk = clock();
    RetValue1 = sat_solver_solve( p->pSat, Vec_IntArray(p->vCondAssump), Vec_IntArray(p->vCondAssump) + Vec_IntSize(p->vCondAssump), 
        (ABC_INT64_T)p->nConfMax, (ABC_INT64_T)0, (ABC_INT64_T)0, (ABC_INT64_T)0 );
    Vec_IntShrink( p->vCondAssump, Vec_IntSize(p->vCondAssump) - 2 );
p->timeSat += clock() - clk;
    if ( RetValue1 == l_False )
    {
        pLitsSat[0] = Abc_LitNot( pLitsSat[0] );
        pLitsSat[1] = Abc_LitNot( pLitsSat[1] );
        RetValue = sat_solver_addclause( p->pSat, pLitsSat, pLitsSat + 2 );
        assert( RetValue );
        pLitsSat[0] = Abc_LitNot( pLitsSat[0] );
        pLitsSat[1] = Abc_LitNot( pLitsSat[1] );
p->timeSatUnsat += clock() - clk;
        p->nSatCallsUnsat++;
    }
    else if ( RetValue1 == l_True )
    {
        p->vCexUser = Gia_ManGetCex( p->pGia, p->vId2Lit, p->pSat, p->vCexSwp );
p->timeSatSat += clock() - clk;
        p->nSatCallsSat++;
        return 0;
    }
    else // if ( RetValue1 == l_Undef )
    {
p->timeSatUndec += clock() - clk;
        p->nSatFails++;
        return -1;
    }
    // return SAT proof
    p->nSatProofs++;
    return 1;
}

/**Function*************************************************************

  Synopsis    [Returns 1 if the set of conditions is UNSAT (0 if SAT; -1 if undecided).]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Gia_SweeperCondCheckUnsat( Gia_Man_t * pGia )
{
    Swp_Man_t * p = (Swp_Man_t *)pGia->pData;
    int RetValue, iLitAig, i;
    clock_t clk;
    p->nSatCalls++;
    assert( p->pSat != NULL );
    p->vCexUser = NULL;

    Vec_IntClear( p->vCondAssump );
    Vec_IntForEachEntry( p->vCondLits, iLitAig, i )
    {
        Gia_ManCnfNodeAddToSolver( p, Abc_Lit2Var(iLitAig) );
        Vec_IntPush( p->vCondAssump, Swp_ManLit2Lit(p, iLitAig) );
    }
    sat_solver_compress( p->pSat );

clk = clock();
    RetValue = sat_solver_solve( p->pSat, Vec_IntArray(p->vCondAssump), Vec_IntArray(p->vCondAssump) + Vec_IntSize(p->vCondAssump), 
        (ABC_INT64_T)p->nConfMax, (ABC_INT64_T)0, (ABC_INT64_T)0, (ABC_INT64_T)0 );
p->timeSat += clock() - clk;
    if ( RetValue == l_False )
    {
        assert( Vec_IntSize(p->vCondLits) > 0 );
p->timeSatUnsat += clock() - clk;
        p->nSatCallsUnsat++;
        return 1;
    }
    else if ( RetValue == l_True )
    {
        p->vCexUser = Gia_ManGetCex( p->pGia, p->vId2Lit, p->pSat, p->vCexSwp );
p->timeSatSat += clock() - clk;
        p->nSatCallsSat++;
        return 0;
    }
    else // if ( RetValue1 == l_Undef )
    {
p->timeSatUndec += clock() - clk;
        p->nSatFails++;
        return -1;
    }
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////

ABC_NAMESPACE_IMPL_END
