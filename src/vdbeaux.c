/*
** 2003 September 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used for creating, destroying, and populating
** a VDBE (or an "sqlite3_stmt" as it is known to the outside world.)  Prior
** to version 2.8.7, all this code was combined into the vdbe.c source file.
** But that file was getting too big so this subroutines were split out.
*/
#include "sqliteInt.h"
#include "os.h"
#include <ctype.h>
#include "vdbeInt.h"


/*
** When debugging the code generator in a symbolic debugger, one can
** set the sqlite3_vdbe_addop_trace to 1 and all opcodes will be printed
** as they are added to the instruction stream.
*/
#ifdef SQLITE_DEBUG
int sqlite3_vdbe_addop_trace = 0;
#endif


/*
** Create a new virtual database engine.
*/
Vdbe *sqlite3VdbeCreate(sqlite3 *db){
  Vdbe *p;
  p = sqliteMalloc( sizeof(Vdbe) );
  if( p==0 ) return 0;
  p->db = db;
  if( db->pVdbe ){
    db->pVdbe->pPrev = p;
  }
  p->pNext = db->pVdbe;
  p->pPrev = 0;
  db->pVdbe = p;
  p->magic = VDBE_MAGIC_INIT;
  return p;
}

/*
** Turn tracing on or off
*/
void sqlite3VdbeTrace(Vdbe *p, FILE *trace){
  p->trace = trace;
}

/*
** Resize the Vdbe.aOp array so that it contains at least N
** elements. If the Vdbe is in VDBE_MAGIC_RUN state, then
** the Vdbe.aOp array will be sized to contain exactly N 
** elements.
*/
static void resizeOpArray(Vdbe *p, int N){
  if( p->magic==VDBE_MAGIC_RUN ){
    assert( N==p->nOp );
    p->nOpAlloc = N;
    p->aOp = sqliteRealloc(p->aOp, N*sizeof(Op));
  }else if( p->nOpAlloc<N ){
    int oldSize = p->nOpAlloc;
    p->nOpAlloc = N+100;
    p->aOp = sqliteRealloc(p->aOp, p->nOpAlloc*sizeof(Op));
    if( p->aOp ){
      memset(&p->aOp[oldSize], 0, (p->nOpAlloc-oldSize)*sizeof(Op));
    }
  }
}

/*
** Add a new instruction to the list of instructions current in the
** VDBE.  Return the address of the new instruction.
**
** Parameters:
**
**    p               Pointer to the VDBE
**
**    op              The opcode for this instruction
**
**    p1, p2          First two of the three possible operands.
**
** Use the sqlite3VdbeResolveLabel() function to fix an address and
** the sqlite3VdbeChangeP3() function to change the value of the P3
** operand.
*/
int sqlite3VdbeAddOp(Vdbe *p, int op, int p1, int p2){
  int i;
  VdbeOp *pOp;

  i = p->nOp;
  p->nOp++;
  assert( p->magic==VDBE_MAGIC_INIT );
  resizeOpArray(p, i+1);
  if( p->aOp==0 ){
    return 0;
  }
  pOp = &p->aOp[i];
  pOp->opcode = op;
  pOp->p1 = p1;
  pOp->p2 = p2;
  pOp->p3 = 0;
  pOp->p3type = P3_NOTUSED;
  p->expired = 0;
#ifdef SQLITE_DEBUG
  if( sqlite3_vdbe_addop_trace ) sqlite3VdbePrintOp(0, i, &p->aOp[i]);
#endif
  return i;
}

/*
** Add an opcode that includes the p3 value.
*/
int sqlite3VdbeOp3(Vdbe *p, int op, int p1, int p2, const char *zP3,int p3type){
  int addr = sqlite3VdbeAddOp(p, op, p1, p2);
  sqlite3VdbeChangeP3(p, addr, zP3, p3type);
  return addr;
}

/*
** Create a new symbolic label for an instruction that has yet to be
** coded.  The symbolic label is really just a negative number.  The
** label can be used as the P2 value of an operation.  Later, when
** the label is resolved to a specific address, the VDBE will scan
** through its operation list and change all values of P2 which match
** the label into the resolved address.
**
** The VDBE knows that a P2 value is a label because labels are
** always negative and P2 values are suppose to be non-negative.
** Hence, a negative P2 value is a label that has yet to be resolved.
**
** Zero is returned if a malloc() fails.
*/
int sqlite3VdbeMakeLabel(Vdbe *p){
  int i;
  i = p->nLabel++;
  assert( p->magic==VDBE_MAGIC_INIT );
  if( i>=p->nLabelAlloc ){
    p->nLabelAlloc = p->nLabelAlloc*2 + 10;
    p->aLabel = sqliteRealloc( p->aLabel, p->nLabelAlloc*sizeof(p->aLabel[0]));
  }
  if( p->aLabel ){
    p->aLabel[i] = -1;
  }
  return -1-i;
}

/*
** Resolve label "x" to be the address of the next instruction to
** be inserted.  The parameter "x" must have been obtained from
** a prior call to sqlite3VdbeMakeLabel().
*/
void sqlite3VdbeResolveLabel(Vdbe *p, int x){
  int j = -1-x;
  assert( p->magic==VDBE_MAGIC_INIT );
  assert( j>=0 && j<p->nLabel );
  if( p->aLabel ){
    p->aLabel[j] = p->nOp;
  }
}

/*
** Return non-zero if opcode 'op' is guarenteed not to push more values
** onto the VDBE stack than it pops off.
*/
static int opcodeNoPush(u8 op){
  /* The 10 NOPUSH_MASK_n constants are defined in the automatically
  ** generated header file opcodes.h. Each is a 16-bit bitmask, one
  ** bit corresponding to each opcode implemented by the virtual
  ** machine in vdbe.c. The bit is true if the word "no-push" appears
  ** in a comment on the same line as the "case OP_XXX:" in 
  ** sqlite3VdbeExec() in vdbe.c.
  **
  ** If the bit is true, then the corresponding opcode is guarenteed not
  ** to grow the stack when it is executed. Otherwise, it may grow the
  ** stack by at most one entry.
  **
  ** NOPUSH_MASK_0 corresponds to opcodes 0 to 15. NOPUSH_MASK_1 contains
  ** one bit for opcodes 16 to 31, and so on.
  **
  ** 16-bit bitmasks (rather than 32-bit) are specified in opcodes.h 
  ** because the file is generated by an awk program. Awk manipulates
  ** all numbers as floating-point and we don't want to risk a rounding
  ** error if someone builds with an awk that uses (for example) 32-bit 
  ** IEEE floats.
  */ 
  static const u32 masks[5] = {
    NOPUSH_MASK_0 + (NOPUSH_MASK_1<<16),
    NOPUSH_MASK_2 + (NOPUSH_MASK_3<<16),
    NOPUSH_MASK_4 + (NOPUSH_MASK_5<<16),
    NOPUSH_MASK_6 + (NOPUSH_MASK_7<<16),
    NOPUSH_MASK_8 + (NOPUSH_MASK_9<<16)
  };
  return (masks[op>>5] & (1<<(op&0x1F)));
}

#ifndef NDEBUG
int sqlite3VdbeOpcodeNoPush(u8 op){
  return opcodeNoPush(op);
}
#endif

/*
** Loop through the program looking for P2 values that are negative.
** Each such value is a label.  Resolve the label by setting the P2
** value to its correct non-zero value.
**
** This routine is called once after all opcodes have been inserted.
**
** Variable *pMaxFuncArgs is set to the maximum value of any P1 argument 
** to an OP_Function or P2 to an OP_AggFunc opcode. This is used by 
** sqlite3VdbeMakeReady() to size the Vdbe.apArg[] array.
**
** The integer *pMaxStack is set to the maximum number of vdbe stack
** entries that static analysis reveals this program might need.
**
** This routine also does the following optimization:  It scans for
** Halt instructions where P1==SQLITE_CONSTRAINT or P2==OE_Abort or for
** IdxInsert instructions where P2!=0.  If no such instruction is
** found, then every Statement instruction is changed to a Noop.  In
** this way, we avoid creating the statement journal file unnecessarily.
*/
static void resolveP2Values(Vdbe *p, int *pMaxFuncArgs, int *pMaxStack){
  int i;
  int nMaxArgs = 0;
  int nMaxStack = p->nOp;
  Op *pOp;
  int *aLabel = p->aLabel;
  int doesStatementRollback = 0;
  int hasStatementBegin = 0;
  for(pOp=p->aOp, i=p->nOp-1; i>=0; i--, pOp++){
    u8 opcode = pOp->opcode;

    /* Todo: Maybe OP_AggFunc should change to use P1 in the same
     * way as OP_Function. 
     */
    if( opcode==OP_Function ){
      if( pOp->p1>nMaxArgs ) nMaxArgs = pOp->p1;
    }else if( opcode==OP_AggFunc ){
      if( pOp->p2>nMaxArgs ) nMaxArgs = pOp->p2;
    }else if( opcode==OP_Halt ){
      if( pOp->p1==SQLITE_CONSTRAINT && pOp->p2==OE_Abort ){
        doesStatementRollback = 1;
      }
    }else if( opcode==OP_IdxInsert ){
      if( pOp->p2 ){
        doesStatementRollback = 1;
      }
    }else if( opcode==OP_Statement ){
      hasStatementBegin = 1;
    }

    if( opcodeNoPush(opcode) ){
      nMaxStack--;
    }

    if( pOp->p2>=0 ) continue;
    assert( -1-pOp->p2<p->nLabel );
    pOp->p2 = aLabel[-1-pOp->p2];
  }
  sqliteFree(p->aLabel);
  p->aLabel = 0;

  *pMaxFuncArgs = nMaxArgs;
  *pMaxStack = nMaxStack;

  /* If we never rollback a statement transaction, then statement
  ** transactions are not needed.  So change every OP_Statement
  ** opcode into an OP_Noop.  This avoid a call to sqlite3OsOpenExclusive()
  ** which can be expensive on some platforms.
  */
  if( hasStatementBegin && !doesStatementRollback ){
    for(pOp=p->aOp, i=p->nOp-1; i>=0; i--, pOp++){
      if( pOp->opcode==OP_Statement ){
        pOp->opcode = OP_Noop;
      }
    }
  }
}

/*
** Return the address of the next instruction to be inserted.
*/
int sqlite3VdbeCurrentAddr(Vdbe *p){
  assert( p->magic==VDBE_MAGIC_INIT );
  return p->nOp;
}

/*
** Add a whole list of operations to the operation stack.  Return the
** address of the first operation added.
*/
int sqlite3VdbeAddOpList(Vdbe *p, int nOp, VdbeOpList const *aOp){
  int addr;
  assert( p->magic==VDBE_MAGIC_INIT );
  resizeOpArray(p, p->nOp + nOp);
  if( p->aOp==0 ){
    return 0;
  }
  addr = p->nOp;
  if( nOp>0 ){
    int i;
    VdbeOpList const *pIn = aOp;
    for(i=0; i<nOp; i++, pIn++){
      int p2 = pIn->p2;
      VdbeOp *pOut = &p->aOp[i+addr];
      pOut->opcode = pIn->opcode;
      pOut->p1 = pIn->p1;
      pOut->p2 = p2<0 ? addr + ADDR(p2) : p2;
      pOut->p3 = pIn->p3;
      pOut->p3type = pIn->p3 ? P3_STATIC : P3_NOTUSED;
#ifdef SQLITE_DEBUG
      if( sqlite3_vdbe_addop_trace ){
        sqlite3VdbePrintOp(0, i+addr, &p->aOp[i+addr]);
      }
#endif
    }
    p->nOp += nOp;
  }
  return addr;
}

/*
** Change the value of the P1 operand for a specific instruction.
** This routine is useful when a large program is loaded from a
** static array using sqlite3VdbeAddOpList but we want to make a
** few minor changes to the program.
*/
void sqlite3VdbeChangeP1(Vdbe *p, int addr, int val){
  assert( p->magic==VDBE_MAGIC_INIT );
  if( p && addr>=0 && p->nOp>addr && p->aOp ){
    p->aOp[addr].p1 = val;
  }
}

/*
** Change the value of the P2 operand for a specific instruction.
** This routine is useful for setting a jump destination.
*/
void sqlite3VdbeChangeP2(Vdbe *p, int addr, int val){
  assert( val>=0 );
  assert( p->magic==VDBE_MAGIC_INIT );
  if( p && addr>=0 && p->nOp>addr && p->aOp ){
    p->aOp[addr].p2 = val;
  }
}

/*
** Change the value of the P3 operand for a specific instruction.
** This routine is useful when a large program is loaded from a
** static array using sqlite3VdbeAddOpList but we want to make a
** few minor changes to the program.
**
** If n>=0 then the P3 operand is dynamic, meaning that a copy of
** the string is made into memory obtained from sqliteMalloc().
** A value of n==0 means copy bytes of zP3 up to and including the
** first null byte.  If n>0 then copy n+1 bytes of zP3.
**
** If n==P3_KEYINFO it means that zP3 is a pointer to a KeyInfo structure.
** A copy is made of the KeyInfo structure into memory obtained from
** sqliteMalloc, to be freed when the Vdbe is finalized.
** n==P3_KEYINFO_HANDOFF indicates that zP3 points to a KeyInfo structure
** stored in memory that the caller has obtained from sqliteMalloc. The 
** caller should not free the allocation, it will be freed when the Vdbe is
** finalized.
** 
** Other values of n (P3_STATIC, P3_COLLSEQ etc.) indicate that zP3 points
** to a string or structure that is guaranteed to exist for the lifetime of
** the Vdbe. In these cases we can just copy the pointer.
**
** If addr<0 then change P3 on the most recently inserted instruction.
*/
void sqlite3VdbeChangeP3(Vdbe *p, int addr, const char *zP3, int n){
  Op *pOp;
  assert( p->magic==VDBE_MAGIC_INIT );
  if( p==0 || p->aOp==0 ){
    if( n==P3_DYNAMIC || n==P3_KEYINFO_HANDOFF ){
      sqliteFree((void*)zP3);
    }
    if( n==P3_MEM ){
      sqlite3ValueFree((sqlite3_value *)zP3);
    }
    return;
  }
  if( addr<0 || addr>=p->nOp ){
    addr = p->nOp - 1;
    if( addr<0 ) return;
  }
  pOp = &p->aOp[addr];
  if( pOp->p3 && pOp->p3type==P3_DYNAMIC ){
    sqliteFree(pOp->p3);
    pOp->p3 = 0;
  }
  if( zP3==0 ){
    pOp->p3 = 0;
    pOp->p3type = P3_NOTUSED;
  }else if( n==P3_KEYINFO ){
    KeyInfo *pKeyInfo;
    int nField, nByte;
    nField = ((KeyInfo*)zP3)->nField;
    nByte = sizeof(*pKeyInfo) + (nField-1)*sizeof(pKeyInfo->aColl[0]);
    pKeyInfo = sqliteMallocRaw( nByte );
    pOp->p3 = (char*)pKeyInfo;
    if( pKeyInfo ){
      memcpy(pKeyInfo, zP3, nByte);
      pOp->p3type = P3_KEYINFO;
    }else{
      pOp->p3type = P3_NOTUSED;
    }
  }else if( n==P3_KEYINFO_HANDOFF ){
    pOp->p3 = (char*)zP3;
    pOp->p3type = P3_KEYINFO;
  }else if( n<0 ){
    pOp->p3 = (char*)zP3;
    pOp->p3type = n;
  }else{
    if( n==0 ) n = strlen(zP3);
    pOp->p3 = sqliteStrNDup(zP3, n);
    pOp->p3type = P3_DYNAMIC;
  }
}

#ifndef NDEBUG
/*
** Replace the P3 field of the most recently coded instruction with
** comment text.
*/
void sqlite3VdbeComment(Vdbe *p, const char *zFormat, ...){
  va_list ap;
  assert( p->nOp>0 );
  assert( p->aOp==0 || p->aOp[p->nOp-1].p3==0 );
  va_start(ap, zFormat);
  sqlite3VdbeChangeP3(p, -1, sqlite3VMPrintf(zFormat, ap), P3_DYNAMIC);
  va_end(ap);
}
#endif

/*
** Return the opcode for a given address.
*/
VdbeOp *sqlite3VdbeGetOp(Vdbe *p, int addr){
  assert( p->magic==VDBE_MAGIC_INIT );
  assert( addr>=0 && addr<p->nOp );
  return &p->aOp[addr];
}

#if !defined(SQLITE_OMIT_EXPLAIN) || !defined(NDEBUG) \
     || defined(VDBE_PROFILE) || defined(SQLITE_DEBUG)
/*
** Compute a string that describes the P3 parameter for an opcode.
** Use zTemp for any required temporary buffer space.
*/
static char *displayP3(Op *pOp, char *zTemp, int nTemp){
  char *zP3;
  assert( nTemp>=20 );
  switch( pOp->p3type ){
    case P3_KEYINFO: {
      int i, j;
      KeyInfo *pKeyInfo = (KeyInfo*)pOp->p3;
      sprintf(zTemp, "keyinfo(%d", pKeyInfo->nField);
      i = strlen(zTemp);
      for(j=0; j<pKeyInfo->nField; j++){
        CollSeq *pColl = pKeyInfo->aColl[j];
        if( pColl ){
          int n = strlen(pColl->zName);
          if( i+n>nTemp-6 ){
            strcpy(&zTemp[i],",...");
            break;
          }
          zTemp[i++] = ',';
          if( pKeyInfo->aSortOrder && pKeyInfo->aSortOrder[j] ){
            zTemp[i++] = '-';
          }
          strcpy(&zTemp[i], pColl->zName);
          i += n;
        }else if( i+4<nTemp-6 ){
          strcpy(&zTemp[i],",nil");
          i += 4;
        }
      }
      zTemp[i++] = ')';
      zTemp[i] = 0;
      assert( i<nTemp );
      zP3 = zTemp;
      break;
    }
    case P3_COLLSEQ: {
      CollSeq *pColl = (CollSeq*)pOp->p3;
      sprintf(zTemp, "collseq(%.20s)", pColl->zName);
      zP3 = zTemp;
      break;
    }
    case P3_FUNCDEF: {
      FuncDef *pDef = (FuncDef*)pOp->p3;
      char zNum[30];
      sprintf(zTemp, "%.*s", nTemp, pDef->zName);
      sprintf(zNum,"(%d)", pDef->nArg);
      if( strlen(zTemp)+strlen(zNum)+1<=nTemp ){
        strcat(zTemp, zNum);
      }
      zP3 = zTemp;
      break;
    }
    default: {
      zP3 = pOp->p3;
      if( zP3==0 || pOp->opcode==OP_Noop ){
        zP3 = "";
      }
    }
  }
  return zP3;
}
#endif


#if defined(VDBE_PROFILE) || defined(SQLITE_DEBUG)
/*
** Print a single opcode.  This routine is used for debugging only.
*/
void sqlite3VdbePrintOp(FILE *pOut, int pc, Op *pOp){
  char *zP3;
  char zPtr[50];
  static const char *zFormat1 = "%4d %-13s %4d %4d %s\n";
  if( pOut==0 ) pOut = stdout;
  zP3 = displayP3(pOp, zPtr, sizeof(zPtr));
  fprintf(pOut, zFormat1,
      pc, sqlite3OpcodeNames[pOp->opcode], pOp->p1, pOp->p2, zP3);
  fflush(pOut);
}
#endif

/*
** Release an array of N Mem elements
*/
static void releaseMemArray(Mem *p, int N){
  if( p ){
    while( N-->0 ){
      sqlite3VdbeMemRelease(p++);
    }
  }
}

#ifndef SQLITE_OMIT_EXPLAIN
/*
** Give a listing of the program in the virtual machine.
**
** The interface is the same as sqlite3VdbeExec().  But instead of
** running the code, it invokes the callback once for each instruction.
** This feature is used to implement "EXPLAIN".
*/
int sqlite3VdbeList(
  Vdbe *p                   /* The VDBE */
){
  sqlite3 *db = p->db;
  int i;
  int rc = SQLITE_OK;

  assert( p->explain );
  if( p->magic!=VDBE_MAGIC_RUN ) return SQLITE_MISUSE;
  assert( db->magic==SQLITE_MAGIC_BUSY );
  assert( p->rc==SQLITE_OK || p->rc==SQLITE_BUSY );

  /* Even though this opcode does not put dynamic strings onto the
  ** the stack, they may become dynamic if the user calls
  ** sqlite3_column_text16(), causing a translation to UTF-16 encoding.
  */
  if( p->pTos==&p->aStack[4] ){
    releaseMemArray(p->aStack, 5);
  }
  p->resOnStack = 0;


  i = p->pc++;
  if( i>=p->nOp ){
    p->rc = SQLITE_OK;
    rc = SQLITE_DONE;
  }else if( db->flags & SQLITE_Interrupt ){
    db->flags &= ~SQLITE_Interrupt;
    p->rc = SQLITE_INTERRUPT;
    rc = SQLITE_ERROR;
    sqlite3SetString(&p->zErrMsg, sqlite3ErrStr(p->rc), (char*)0);
  }else{
    Op *pOp = &p->aOp[i];
    Mem *pMem = p->aStack;
    pMem->flags = MEM_Int;
    pMem->type = SQLITE_INTEGER;
    pMem->i = i;                                /* Program counter */
    pMem++;

    pMem->flags = MEM_Static|MEM_Str|MEM_Term;
    pMem->z = sqlite3OpcodeNames[pOp->opcode];  /* Opcode */
    pMem->n = strlen(pMem->z);
    pMem->type = SQLITE_TEXT;
    pMem->enc = SQLITE_UTF8;
    pMem++;

    pMem->flags = MEM_Int;
    pMem->i = pOp->p1;                          /* P1 */
    pMem->type = SQLITE_INTEGER;
    pMem++;

    pMem->flags = MEM_Int;
    pMem->i = pOp->p2;                          /* P2 */
    pMem->type = SQLITE_INTEGER;
    pMem++;

    pMem->flags = MEM_Short|MEM_Str|MEM_Term;   /* P3 */
    pMem->z = displayP3(pOp, pMem->zShort, sizeof(pMem->zShort));
    pMem->type = SQLITE_TEXT;
    pMem->enc = SQLITE_UTF8;

    p->nResColumn = 5;
    p->pTos = pMem;
    p->rc = SQLITE_OK;
    p->resOnStack = 1;
    rc = SQLITE_ROW;
  }
  return rc;
}
#endif /* SQLITE_OMIT_EXPLAIN */

/*
** Print the SQL that was used to generate a VDBE program.
*/
void sqlite3VdbePrintSql(Vdbe *p){
#ifdef SQLITE_DEBUG
  int nOp = p->nOp;
  VdbeOp *pOp;
  if( nOp<1 ) return;
  pOp = &p->aOp[nOp-1];
  if( pOp->opcode==OP_Noop && pOp->p3!=0 ){
    const char *z = pOp->p3;
    while( isspace(*(u8*)z) ) z++;
    printf("SQL: [%s]\n", z);
  }
#endif
}

/*
** Prepare a virtual machine for execution.  This involves things such
** as allocating stack space and initializing the program counter.
** After the VDBE has be prepped, it can be executed by one or more
** calls to sqlite3VdbeExec().  
**
** This is the only way to move a VDBE from VDBE_MAGIC_INIT to
** VDBE_MAGIC_RUN.
*/
void sqlite3VdbeMakeReady(
  Vdbe *p,                       /* The VDBE */
  int nVar,                      /* Number of '?' see in the SQL statement */
  int nMem,                      /* Number of memory cells to allocate */
  int nCursor,                   /* Number of cursors to allocate */
  int nAgg,                      /* Number of aggregate contexts required */
  int isExplain                  /* True if the EXPLAIN keywords is present */
){
  int n;

  assert( p!=0 );
  assert( p->magic==VDBE_MAGIC_INIT );

  /* There should be at least one opcode.
  */
  assert( p->nOp>0 );

  /* Set the magic to VDBE_MAGIC_RUN sooner rather than later. This
   * is because the call to resizeOpArray() below may shrink the
   * p->aOp[] array to save memory if called when in VDBE_MAGIC_RUN 
   * state.
   */
  p->magic = VDBE_MAGIC_RUN;

  /* No instruction ever pushes more than a single element onto the
  ** stack.  And the stack never grows on successive executions of the
  ** same loop.  So the total number of instructions is an upper bound
  ** on the maximum stack depth required.  (Added later:)  The
  ** resolveP2Values() call computes a tighter upper bound on the
  ** stack size.
  **
  ** Allocation all the stack space we will ever need.
  */
  if( p->aStack==0 ){
    int nArg;       /* Maximum number of args passed to a user function. */
    int nStack;     /* Maximum number of stack entries required */
    resolveP2Values(p, &nArg, &nStack);
    resizeOpArray(p, p->nOp);
    assert( nVar>=0 );
    assert( nStack<p->nOp );
    nStack = isExplain ? 10 : nStack;
    p->aStack = sqliteMalloc(
        nStack*sizeof(p->aStack[0])    /* aStack */
      + nArg*sizeof(Mem*)              /* apArg */
      + nVar*sizeof(Mem)               /* aVar */
      + nVar*sizeof(char*)             /* azVar */
      + nMem*sizeof(Mem)               /* aMem */
      + nCursor*sizeof(Cursor*)        /* apCsr */
      + nAgg*sizeof(Agg)               /* Aggregate contexts */
    );
    if( !sqlite3_malloc_failed ){
      p->aMem = &p->aStack[nStack];
      p->nMem = nMem;
      p->aVar = &p->aMem[nMem];
      p->nVar = nVar;
      p->okVar = 0;
      p->apArg = (Mem**)&p->aVar[nVar];
      p->azVar = (char**)&p->apArg[nArg];
      p->apCsr = (Cursor**)&p->azVar[nVar];
      if( nAgg>0 ){
        p->nAgg = nAgg;
        p->apAgg = (Agg*)&p->apCsr[nCursor];
      }
      p->nCursor = nCursor;
      for(n=0; n<nVar; n++){
        p->aVar[n].flags = MEM_Null;
      }
    }
  }
  p->pAgg = p->apAgg;
  for(n=0; n<p->nMem; n++){
    p->aMem[n].flags = MEM_Null;
  }

#ifdef SQLITE_DEBUG
  if( (p->db->flags & SQLITE_VdbeListing)!=0
    || sqlite3OsFileExists("vdbe_explain")
  ){
    int i;
    printf("VDBE Program Listing:\n");
    sqlite3VdbePrintSql(p);
    for(i=0; i<p->nOp; i++){
      sqlite3VdbePrintOp(stdout, i, &p->aOp[i]);
    }
  }
  if( sqlite3OsFileExists("vdbe_trace") ){
    p->trace = stdout;
  }
#endif
  p->pTos = &p->aStack[-1];
  p->pc = -1;
  p->rc = SQLITE_OK;
  p->uniqueCnt = 0;
  p->returnDepth = 0;
  p->errorAction = OE_Abort;
  p->popStack =  0;
  p->explain |= isExplain;
  p->magic = VDBE_MAGIC_RUN;
  p->nChange = 0;
#ifdef VDBE_PROFILE
  {
    int i;
    for(i=0; i<p->nOp; i++){
      p->aOp[i].cnt = 0;
      p->aOp[i].cycles = 0;
    }
  }
#endif
}


/*
** Remove any elements that remain on the sorter for the VDBE given.
*/
void sqlite3VdbeSorterReset(Vdbe *p){
  while( p->pSort ){
    Sorter *pSorter = p->pSort;
    p->pSort = pSorter->pNext;
    sqliteFree(pSorter->zKey);
    sqlite3VdbeMemRelease(&pSorter->data);
    sqliteFree(pSorter);
  }
  p->pSortTail = 0;
}

/*
** Free all resources allociated with AggElem pElem, an element of
** aggregate pAgg.
*/
static void freeAggElem(AggElem *pElem, Agg *pAgg){
  int i;
  for(i=0; i<pAgg->nMem; i++){
    Mem *pMem = &pElem->aMem[i];
    if( pAgg->apFunc && pAgg->apFunc[i] && (pMem->flags & MEM_AggCtx)!=0 ){
      sqlite3_context ctx;
      ctx.pFunc = pAgg->apFunc[i];
      ctx.s.flags = MEM_Null;
      ctx.pAgg = pMem->z;
      ctx.cnt = pMem->i;
      ctx.isError = 0;
      (*ctx.pFunc->xFinalize)(&ctx);
      pMem->z = ctx.pAgg;
      if( pMem->z!=0 && pMem->z!=pMem->zShort ){
        sqliteFree(pMem->z);
      }
      sqlite3VdbeMemRelease(&ctx.s);
    }else{
      sqlite3VdbeMemRelease(pMem);
    }
  }
  sqliteFree(pElem);
}

/*
** Reset an Agg structure.  Delete all its contents.
**
** For installable aggregate functions, if the step function has been
** called, make sure the finalizer function has also been called.  The
** finalizer might need to free memory that was allocated as part of its
** private context.  If the finalizer has not been called yet, call it
** now.
**
** If db is NULL, then this is being called from sqliteVdbeReset(). In
** this case clean up all references to the temp-table used for
** aggregates (if it was ever opened).
**
** If db is not NULL, then this is being called from with an OP_AggReset
** opcode. Open the temp-table, if it has not already been opened and
** delete the contents of the table used for aggregate information, ready
** for the next round of aggregate processing.
*/
int sqlite3VdbeAggReset(sqlite3 *db, Agg *pAgg, KeyInfo *pKeyInfo){
  int rc = 0;
  BtCursor *pCsr;

  if( !pAgg ) return SQLITE_OK;
  pCsr = pAgg->pCsr;
  assert( (pCsr && pAgg->nTab>0) || (!pCsr && pAgg->nTab==0)
         || sqlite3_malloc_failed );

  /* If pCsr is not NULL, then the table used for aggregate information
  ** is open. Loop through it and free the AggElem* structure pointed at
  ** by each entry. If the finalizer has not been called for an AggElem,
  ** do that too. Finally, clear the btree table itself.
  */
  if( pCsr ){
    int res;
    assert( pAgg->pBtree );
    assert( pAgg->nTab>0 );

    rc=sqlite3BtreeFirst(pCsr, &res);
    while( res==0 && rc==SQLITE_OK ){
      AggElem *pElem;
      rc = sqlite3BtreeData(pCsr, 0, sizeof(AggElem*), (char *)&pElem);
      if( rc!=SQLITE_OK ){
        return rc;
      }
      assert( pAgg->apFunc!=0 );
      freeAggElem(pElem, pAgg);
      rc=sqlite3BtreeNext(pCsr, &res);
    }
    if( rc!=SQLITE_OK ){
      return rc;
    }

    sqlite3BtreeCloseCursor(pCsr);
    sqlite3BtreeClearTable(pAgg->pBtree, pAgg->nTab);
  }else{ 
    /* The cursor may not be open because the aggregator was never used,
    ** or it could be that it was used but there was no GROUP BY clause.
    */
    if( pAgg->pCurrent ){
      freeAggElem(pAgg->pCurrent, pAgg);
    }
  }

  /* If db is not NULL and we have not yet and we have not yet opened
  ** the temporary btree then do so and create the table to store aggregate
  ** information.
  **
  ** If db is NULL, then close the temporary btree if it is open.
  */
  if( db ){
    if( !pAgg->pBtree ){
      assert( pAgg->nTab==0 );
#ifndef SQLITE_OMIT_MEMORYDB
      rc = sqlite3BtreeFactory(db, ":memory:", 0, TEMP_PAGES, &pAgg->pBtree);
#else
      rc = sqlite3BtreeFactory(db, 0, 0, TEMP_PAGES, &pAgg->pBtree);
#endif
      if( rc!=SQLITE_OK ) return rc;
      sqlite3BtreeBeginTrans(pAgg->pBtree, 1);
      rc = sqlite3BtreeCreateTable(pAgg->pBtree, &pAgg->nTab, 0);
      if( rc!=SQLITE_OK ) return rc;
    }
    assert( pAgg->nTab!=0 );

    rc = sqlite3BtreeCursor(pAgg->pBtree, pAgg->nTab, 1,
        sqlite3VdbeRecordCompare, pKeyInfo, &pAgg->pCsr);
    if( rc!=SQLITE_OK ) return rc;
  }else{
    if( pAgg->pBtree ){
      sqlite3BtreeClose(pAgg->pBtree);
      pAgg->pBtree = 0;
      pAgg->nTab = 0;
    }
    pAgg->pCsr = 0;
  }

  if( pAgg->apFunc ){ 
    sqliteFree(pAgg->apFunc);
    pAgg->apFunc = 0;
  }
  pAgg->pCurrent = 0;
  pAgg->nMem = 0;
  pAgg->searching = 0;
  return SQLITE_OK;
}

/*
** Close a cursor and release all the resources that cursor happens
** to hold.
*/
void sqlite3VdbeFreeCursor(Cursor *pCx){
  if( pCx==0 ){
    return;
  }
  if( pCx->pCursor ){
    sqlite3BtreeCloseCursor(pCx->pCursor);
  }
  if( pCx->pBt ){
    sqlite3BtreeClose(pCx->pBt);
  }
  sqliteFree(pCx->pData);
  sqliteFree(pCx->aType);
  sqliteFree(pCx);
}

/*
** Close all cursors
*/
static void closeAllCursors(Vdbe *p){
  int i;
  if( p->apCsr==0 ) return;
  for(i=0; i<p->nCursor; i++){
    sqlite3VdbeFreeCursor(p->apCsr[i]);
    p->apCsr[i] = 0;
  }
}

/*
** Clean up the VM after execution.
**
** This routine will automatically close any cursors, lists, and/or
** sorters that were left open.  It also deletes the values of
** variables in the aVar[] array.
*/
static void Cleanup(Vdbe *p){
  int i;
  if( p->aStack ){
    releaseMemArray(p->aStack, 1 + (p->pTos - p->aStack));
    p->pTos = &p->aStack[-1];
  }
  closeAllCursors(p);
  releaseMemArray(p->aMem, p->nMem);
  sqlite3VdbeFifoClear(&p->sFifo);
  if( p->contextStack ){
    for(i=0; i<p->contextStackTop; i++){
      sqlite3VdbeFifoClear(&p->contextStack[i].sFifo);
    }
    sqliteFree(p->contextStack);
  }
  sqlite3VdbeSorterReset(p);
  for(i=0; i<p->nAgg; i++){
    sqlite3VdbeAggReset(0, &p->apAgg[i], 0);
  }
  p->contextStack = 0;
  p->contextStackDepth = 0;
  p->contextStackTop = 0;
  sqliteFree(p->zErrMsg);
  p->zErrMsg = 0;
}

/*
** Set the number of result columns that will be returned by this SQL
** statement. This is now set at compile time, rather than during
** execution of the vdbe program so that sqlite3_column_count() can
** be called on an SQL statement before sqlite3_step().
*/
void sqlite3VdbeSetNumCols(Vdbe *p, int nResColumn){
  Mem *pColName;
  int n;
  assert( 0==p->nResColumn );
  p->nResColumn = nResColumn;
  n = nResColumn*2;
  p->aColName = pColName = (Mem*)sqliteMalloc( sizeof(Mem)*n );
  if( p->aColName==0 ) return;
  while( n-- > 0 ){
    (pColName++)->flags = MEM_Null;
  }
}

/*
** Set the name of the idx'th column to be returned by the SQL statement.
** zName must be a pointer to a nul terminated string.
**
** This call must be made after a call to sqlite3VdbeSetNumCols().
**
** If N==P3_STATIC  it means that zName is a pointer to a constant static
** string and we can just copy the pointer. If it is P3_DYNAMIC, then 
** the string is freed using sqliteFree() when the vdbe is finished with
** it. Otherwise, N bytes of zName are copied.
*/
int sqlite3VdbeSetColName(Vdbe *p, int idx, const char *zName, int N){
  int rc;
  Mem *pColName;
  assert( idx<(2*p->nResColumn) );
  if( sqlite3_malloc_failed ) return SQLITE_NOMEM;
  assert( p->aColName!=0 );
  pColName = &(p->aColName[idx]);
  if( N==P3_DYNAMIC || N==P3_STATIC ){
    rc = sqlite3VdbeMemSetStr(pColName, zName, -1, SQLITE_UTF8, SQLITE_STATIC);
  }else{
    rc = sqlite3VdbeMemSetStr(pColName, zName, N, SQLITE_UTF8,SQLITE_TRANSIENT);
  }
  if( rc==SQLITE_OK && N==P3_DYNAMIC ){
    pColName->flags = (pColName->flags&(~MEM_Static))|MEM_Dyn;
    pColName->xDel = 0;
  }
  return rc;
}

/*
** A read or write transaction may or may not be active on database handle
** db. If a transaction is active, commit it. If there is a
** write-transaction spanning more than one database file, this routine
** takes care of the master journal trickery.
*/
static int vdbeCommit(sqlite3 *db){
  int i;
  int nTrans = 0;  /* Number of databases with an active write-transaction */
  int rc = SQLITE_OK;
  int needXcommit = 0;

  for(i=0; i<db->nDb; i++){ 
    Btree *pBt = db->aDb[i].pBt;
    if( pBt && sqlite3BtreeIsInTrans(pBt) ){
      needXcommit = 1;
      if( i!=1 ) nTrans++;
    }
  }

  /* If there are any write-transactions at all, invoke the commit hook */
  if( needXcommit && db->xCommitCallback ){
    int rc;
    sqlite3SafetyOff(db);
    rc = db->xCommitCallback(db->pCommitArg);
    sqlite3SafetyOn(db);
    if( rc ){
      return SQLITE_CONSTRAINT;
    }
  }

  /* The simple case - no more than one database file (not counting the
  ** TEMP database) has a transaction active.   There is no need for the
  ** master-journal.
  **
  ** If the return value of sqlite3BtreeGetFilename() is a zero length
  ** string, it means the main database is :memory:.  In that case we do
  ** not support atomic multi-file commits, so use the simple case then
  ** too.
  */
  if( 0==strlen(sqlite3BtreeGetFilename(db->aDb[0].pBt)) || nTrans<=1 ){
    for(i=0; rc==SQLITE_OK && i<db->nDb; i++){ 
      Btree *pBt = db->aDb[i].pBt;
      if( pBt ){
        rc = sqlite3BtreeSync(pBt, 0);
      }
    }

    /* Do the commit only if all databases successfully synced */
    if( rc==SQLITE_OK ){
      for(i=0; i<db->nDb; i++){
        Btree *pBt = db->aDb[i].pBt;
        if( pBt ){
          sqlite3BtreeCommit(pBt);
        }
      }
    }
  }

  /* The complex case - There is a multi-file write-transaction active.
  ** This requires a master journal file to ensure the transaction is
  ** committed atomicly.
  */
#ifndef SQLITE_OMIT_DISKIO
  else{
    int needSync = 0;
    char *zMaster = 0;   /* File-name for the master journal */
    char const *zMainFile = sqlite3BtreeGetFilename(db->aDb[0].pBt);
    OsFile master;

    /* Select a master journal file name */
    do {
      u32 random;
      sqliteFree(zMaster);
      sqlite3Randomness(sizeof(random), &random);
      zMaster = sqlite3MPrintf("%s-mj%08X", zMainFile, random&0x7fffffff);
      if( !zMaster ){
        return SQLITE_NOMEM;
      }
    }while( sqlite3OsFileExists(zMaster) );

    /* Open the master journal. */
    memset(&master, 0, sizeof(master));
    rc = sqlite3OsOpenExclusive(zMaster, &master, 0);
    if( rc!=SQLITE_OK ){
      sqliteFree(zMaster);
      return rc;
    }
 
    /* Write the name of each database file in the transaction into the new
    ** master journal file. If an error occurs at this point close
    ** and delete the master journal file. All the individual journal files
    ** still have 'null' as the master journal pointer, so they will roll
    ** back independently if a failure occurs.
    */
    for(i=0; i<db->nDb; i++){ 
      Btree *pBt = db->aDb[i].pBt;
      if( i==1 ) continue;   /* Ignore the TEMP database */
      if( pBt && sqlite3BtreeIsInTrans(pBt) ){
        char const *zFile = sqlite3BtreeGetJournalname(pBt);
        if( zFile[0]==0 ) continue;  /* Ignore :memory: databases */
        if( !needSync && !sqlite3BtreeSyncDisabled(pBt) ){
          needSync = 1;
        }
        rc = sqlite3OsWrite(&master, zFile, strlen(zFile)+1);
        if( rc!=SQLITE_OK ){
          sqlite3OsClose(&master);
          sqlite3OsDelete(zMaster);
          sqliteFree(zMaster);
          return rc;
        }
      }
    }


    /* Sync the master journal file. Before doing this, open the directory
    ** the master journal file is store in so that it gets synced too.
    */
    zMainFile = sqlite3BtreeGetDirname(db->aDb[0].pBt);
    rc = sqlite3OsOpenDirectory(zMainFile, &master);
    if( rc!=SQLITE_OK || (needSync && (rc=sqlite3OsSync(&master))!=SQLITE_OK) ){
      sqlite3OsClose(&master);
      sqlite3OsDelete(zMaster);
      sqliteFree(zMaster);
      return rc;
    }

    /* Sync all the db files involved in the transaction. The same call
    ** sets the master journal pointer in each individual journal. If
    ** an error occurs here, do not delete the master journal file.
    **
    ** If the error occurs during the first call to sqlite3BtreeSync(),
    ** then there is a chance that the master journal file will be
    ** orphaned. But we cannot delete it, in case the master journal
    ** file name was written into the journal file before the failure
    ** occured.
    */
    for(i=0; i<db->nDb; i++){ 
      Btree *pBt = db->aDb[i].pBt;
      if( pBt && sqlite3BtreeIsInTrans(pBt) ){
        rc = sqlite3BtreeSync(pBt, zMaster);
        if( rc!=SQLITE_OK ){
          sqlite3OsClose(&master);
          sqliteFree(zMaster);
          return rc;
        }
      }
    }
    sqlite3OsClose(&master);

    /* Delete the master journal file. This commits the transaction. After
    ** doing this the directory is synced again before any individual
    ** transaction files are deleted.
    */
    rc = sqlite3OsDelete(zMaster);
    assert( rc==SQLITE_OK );
    sqliteFree(zMaster);
    zMaster = 0;
    rc = sqlite3OsSyncDirectory(zMainFile);
    if( rc!=SQLITE_OK ){
      /* This is not good. The master journal file has been deleted, but
      ** the directory sync failed. There is no completely safe course of
      ** action from here. The individual journals contain the name of the
      ** master journal file, but there is no way of knowing if that
      ** master journal exists now or if it will exist after the operating
      ** system crash that may follow the fsync() failure.
      */
      return rc;
    }

    /* All files and directories have already been synced, so the following
    ** calls to sqlite3BtreeCommit() are only closing files and deleting
    ** journals. If something goes wrong while this is happening we don't
    ** really care. The integrity of the transaction is already guaranteed,
    ** but some stray 'cold' journals may be lying around. Returning an
    ** error code won't help matters.
    */
    for(i=0; i<db->nDb; i++){ 
      Btree *pBt = db->aDb[i].pBt;
      if( pBt ){
        sqlite3BtreeCommit(pBt);
      }
    }
  }
#endif

  return rc;
}

/*
** Find every active VM other than pVdbe and change its status to
** aborted.  This happens when one VM causes a rollback due to an
** ON CONFLICT ROLLBACK clause (for example).  The other VMs must be
** aborted so that they do not have data rolled out from underneath
** them leading to a segfault.
*/
static void abortOtherActiveVdbes(Vdbe *pVdbe){
  Vdbe *pOther;
  for(pOther=pVdbe->db->pVdbe; pOther; pOther=pOther->pNext){
    if( pOther==pVdbe ) continue;
    if( pOther->magic!=VDBE_MAGIC_RUN || pOther->pc<0 ) continue;
    closeAllCursors(pOther);
    pOther->aborted = 1;
  }
}

/* 
** This routine checks that the sqlite3.activeVdbeCnt count variable
** matches the number of vdbe's in the list sqlite3.pVdbe that are
** currently active. An assertion fails if the two counts do not match.
** This is an internal self-check only - it is not an essential processing
** step.
**
** This is a no-op if NDEBUG is defined.
*/
#ifndef NDEBUG
static void checkActiveVdbeCnt(sqlite3 *db){
  Vdbe *p;
  int cnt = 0;
  p = db->pVdbe;
  while( p ){
    if( p->magic==VDBE_MAGIC_RUN && p->pc>=0 ){
      cnt++;
    }
    p = p->pNext;
  }
  assert( cnt==db->activeVdbeCnt );
}
#else
#define checkActiveVdbeCnt(x)
#endif

/*
** This routine is called the when a VDBE tries to halt.  If the VDBE
** has made changes and is in autocommit mode, then commit those
** changes.  If a rollback is needed, then do the rollback.
**
** This routine is the only way to move the state of a VM from
** SQLITE_MAGIC_RUN to SQLITE_MAGIC_HALT.
**
** Return an error code.  If the commit could not complete because of
** lock contention, return SQLITE_BUSY.  If SQLITE_BUSY is returned, it
** means the close did not happen and needs to be repeated.
*/
int sqlite3VdbeHalt(Vdbe *p){
  sqlite3 *db = p->db;
  int i;
  int (*xFunc)(Btree *pBt) = 0;  /* Function to call on each btree backend */

  if( p->magic!=VDBE_MAGIC_RUN ){
    /* Already halted.  Nothing to do. */
    assert( p->magic==VDBE_MAGIC_HALT );
    return SQLITE_OK;
  }
  closeAllCursors(p);
  checkActiveVdbeCnt(db);
  if( p->pc<0 ){
    /* No commit or rollback needed if the program never started */
  }else if( db->autoCommit && db->activeVdbeCnt==1 ){
    if( p->rc==SQLITE_OK || p->errorAction==OE_Fail ){
      /* The auto-commit flag is true, there are no other active queries
      ** using this handle and the vdbe program was successful or hit an
      ** 'OR FAIL' constraint. This means a commit is required.
      */
      int rc = vdbeCommit(db);
      if( rc==SQLITE_BUSY ){
        return SQLITE_BUSY;
      }else if( rc!=SQLITE_OK ){
        p->rc = rc;
        xFunc = sqlite3BtreeRollback;
      }
    }else{
      xFunc = sqlite3BtreeRollback;
    }
  }else{
    if( p->rc==SQLITE_OK || p->errorAction==OE_Fail ){
      xFunc = sqlite3BtreeCommitStmt;
    }else if( p->errorAction==OE_Abort ){
      xFunc = sqlite3BtreeRollbackStmt;
    }else{
      xFunc = sqlite3BtreeRollback;
      db->autoCommit = 1;
      abortOtherActiveVdbes(p);
    }
  }

  /* If xFunc is not NULL, then it is one of sqlite3BtreeRollback,
  ** sqlite3BtreeRollbackStmt or sqlite3BtreeCommitStmt. Call it once on
  ** each backend. If an error occurs and the return code is still
  ** SQLITE_OK, set the return code to the new error value.
  */
  for(i=0; xFunc && i<db->nDb; i++){ 
    int rc;
    Btree *pBt = db->aDb[i].pBt;
    if( pBt ){
      rc = xFunc(pBt);
      if( p->rc==SQLITE_OK ) p->rc = rc;
    }
  }

  /* If this was an INSERT, UPDATE or DELETE, set the change counter. */
  if( p->changeCntOn && p->pc>=0 ){
    if( !xFunc || xFunc==sqlite3BtreeCommitStmt ){
      sqlite3VdbeSetChanges(db, p->nChange);
    }else{
      sqlite3VdbeSetChanges(db, 0);
    }
    p->nChange = 0;
  }

  /* Rollback or commit any schema changes that occurred. */
  if( p->rc!=SQLITE_OK ){
    sqlite3RollbackInternalChanges(db);
  }else if( db->flags & SQLITE_InternChanges ){
    sqlite3CommitInternalChanges(db);
  }

  /* We have successfully halted and closed the VM.  Record this fact. */
  if( p->pc>=0 ){
    db->activeVdbeCnt--;
  }
  p->magic = VDBE_MAGIC_HALT;
  checkActiveVdbeCnt(db);

  return SQLITE_OK;
}

/*
** Clean up a VDBE after execution but do not delete the VDBE just yet.
** Write any error messages into *pzErrMsg.  Return the result code.
**
** After this routine is run, the VDBE should be ready to be executed
** again.
**
** To look at it another way, this routine resets the state of the
** virtual machine from VDBE_MAGIC_RUN or VDBE_MAGIC_HALT back to
** VDBE_MAGIC_INIT.
*/
int sqlite3VdbeReset(Vdbe *p){
  if( p->magic!=VDBE_MAGIC_RUN && p->magic!=VDBE_MAGIC_HALT ){
    sqlite3Error(p->db, SQLITE_MISUSE, 0);
    return SQLITE_MISUSE;
  }

  /* If the VM did not run to completion or if it encountered an
  ** error, then it might not have been halted properly.  So halt
  ** it now.
  */
  sqlite3VdbeHalt(p);

  /* If the VDBE has be run even partially, then transfer the error code
  ** and error message from the VDBE into the main database structure.  But
  ** if the VDBE has just been set to run but has not actually executed any
  ** instructions yet, leave the main database error information unchanged.
  */
  if( p->pc>=0 ){
    if( p->zErrMsg ){
      sqlite3Error(p->db, p->rc, "%s", p->zErrMsg);
      sqliteFree(p->zErrMsg);
      p->zErrMsg = 0;
    }else if( p->rc ){
      sqlite3Error(p->db, p->rc, 0);
    }else{
      sqlite3Error(p->db, SQLITE_OK, 0);
    }
  }else if( p->rc && p->expired ){
    /* The expired flag was set on the VDBE before the first call
    ** to sqlite3_step(). For consistency (since sqlite3_step() was
    ** called), set the database error in this case as well.
    */
    sqlite3Error(p->db, p->rc, 0);
  }

  /* Reclaim all memory used by the VDBE
  */
  Cleanup(p);

  /* Save profiling information from this VDBE run.
  */
  assert( p->pTos<&p->aStack[p->pc<0?0:p->pc] || sqlite3_malloc_failed==1 );
#ifdef VDBE_PROFILE
  {
    FILE *out = fopen("vdbe_profile.out", "a");
    if( out ){
      int i;
      fprintf(out, "---- ");
      for(i=0; i<p->nOp; i++){
        fprintf(out, "%02x", p->aOp[i].opcode);
      }
      fprintf(out, "\n");
      for(i=0; i<p->nOp; i++){
        fprintf(out, "%6d %10lld %8lld ",
           p->aOp[i].cnt,
           p->aOp[i].cycles,
           p->aOp[i].cnt>0 ? p->aOp[i].cycles/p->aOp[i].cnt : 0
        );
        sqlite3VdbePrintOp(out, i, &p->aOp[i]);
      }
      fclose(out);
    }
  }
#endif
  p->magic = VDBE_MAGIC_INIT;
  p->aborted = 0;
  if( p->rc==SQLITE_SCHEMA ){
    sqlite3ResetInternalSchema(p->db, 0);
  }
  return p->rc;
}
 
/*
** Clean up and delete a VDBE after execution.  Return an integer which is
** the result code.  Write any error message text into *pzErrMsg.
*/
int sqlite3VdbeFinalize(Vdbe *p){
  int rc = SQLITE_OK;

  if( p->magic==VDBE_MAGIC_RUN || p->magic==VDBE_MAGIC_HALT ){
    rc = sqlite3VdbeReset(p);
  }else if( p->magic!=VDBE_MAGIC_INIT ){
    return SQLITE_MISUSE;
  }
  sqlite3VdbeDelete(p);
  return rc;
}

/*
** Call the destructor for each auxdata entry in pVdbeFunc for which
** the corresponding bit in mask is clear.  Auxdata entries beyond 31
** are always destroyed.  To destroy all auxdata entries, call this
** routine with mask==0.
*/
void sqlite3VdbeDeleteAuxData(VdbeFunc *pVdbeFunc, int mask){
  int i;
  for(i=0; i<pVdbeFunc->nAux; i++){
    struct AuxData *pAux = &pVdbeFunc->apAux[i];
    if( (i>31 || !(mask&(1<<i))) && pAux->pAux ){
      if( pAux->xDelete ){
        pAux->xDelete(pAux->pAux);
      }
      pAux->pAux = 0;
    }
  }
}

/*
** Delete an entire VDBE.
*/
void sqlite3VdbeDelete(Vdbe *p){
  int i;
  if( p==0 ) return;
  Cleanup(p);
  if( p->pPrev ){
    p->pPrev->pNext = p->pNext;
  }else{
    assert( p->db->pVdbe==p );
    p->db->pVdbe = p->pNext;
  }
  if( p->pNext ){
    p->pNext->pPrev = p->pPrev;
  }
  if( p->aOp ){
    for(i=0; i<p->nOp; i++){
      Op *pOp = &p->aOp[i];
      if( pOp->p3type==P3_DYNAMIC || pOp->p3type==P3_KEYINFO ){
        sqliteFree(pOp->p3);
      }
      if( pOp->p3type==P3_VDBEFUNC ){
        VdbeFunc *pVdbeFunc = (VdbeFunc *)pOp->p3;
        sqlite3VdbeDeleteAuxData(pVdbeFunc, 0);
        sqliteFree(pVdbeFunc);
      }
      if( pOp->p3type==P3_MEM ){
        sqlite3ValueFree((sqlite3_value*)pOp->p3);
      }
    }
    sqliteFree(p->aOp);
  }
  releaseMemArray(p->aVar, p->nVar);
  sqliteFree(p->aLabel);
  sqliteFree(p->aStack);
  releaseMemArray(p->aColName, p->nResColumn*2);
  sqliteFree(p->aColName);
  p->magic = VDBE_MAGIC_DEAD;
  sqliteFree(p);
}

/*
** If a MoveTo operation is pending on the given cursor, then do that
** MoveTo now.  Return an error code.  If no MoveTo is pending, this
** routine does nothing and returns SQLITE_OK.
*/
int sqlite3VdbeCursorMoveto(Cursor *p){
  if( p->deferredMoveto ){
    int res, rc;
    extern int sqlite3_search_count;
    assert( p->isTable );
    if( p->isTable ){
      rc = sqlite3BtreeMoveto(p->pCursor, 0, p->movetoTarget, &res);
    }else{
      rc = sqlite3BtreeMoveto(p->pCursor,(char*)&p->movetoTarget,
                              sizeof(i64),&res);
    }
    if( rc ) return rc;
    *p->pIncrKey = 0;
    p->lastRowid = keyToInt(p->movetoTarget);
    p->rowidIsValid = res==0;
    if( res<0 ){
      rc = sqlite3BtreeNext(p->pCursor, &res);
      if( rc ) return rc;
    }
    sqlite3_search_count++;
    p->deferredMoveto = 0;
    p->cacheValid = 0;
  }
  return SQLITE_OK;
}

/*
** The following functions:
**
** sqlite3VdbeSerialType()
** sqlite3VdbeSerialTypeLen()
** sqlite3VdbeSerialRead()
** sqlite3VdbeSerialLen()
** sqlite3VdbeSerialWrite()
**
** encapsulate the code that serializes values for storage in SQLite
** data and index records. Each serialized value consists of a
** 'serial-type' and a blob of data. The serial type is an 8-byte unsigned
** integer, stored as a varint.
**
** In an SQLite index record, the serial type is stored directly before
** the blob of data that it corresponds to. In a table record, all serial
** types are stored at the start of the record, and the blobs of data at
** the end. Hence these functions allow the caller to handle the
** serial-type and data blob seperately.
**
** The following table describes the various storage classes for data:
**
**   serial type        bytes of data      type
**   --------------     ---------------    ---------------
**      0                     0            NULL
**      1                     1            signed integer
**      2                     2            signed integer
**      3                     3            signed integer
**      4                     4            signed integer
**      5                     6            signed integer
**      6                     8            signed integer
**      7                     8            IEEE float
**     8-11                                reserved for expansion
**    N>=12 and even       (N-12)/2        BLOB
**    N>=13 and odd        (N-13)/2        text
**
*/

/*
** Return the serial-type for the value stored in pMem.
*/
u32 sqlite3VdbeSerialType(Mem *pMem){
  int flags = pMem->flags;

  if( flags&MEM_Null ){
    return 0;
  }
  if( flags&MEM_Int ){
    /* Figure out whether to use 1, 2, 4, 6 or 8 bytes. */
#   define MAX_6BYTE ((((i64)0x00001000)<<32)-1)
    i64 i = pMem->i;
    u64 u = i<0 ? -i : i;
    if( u<=127 ) return 1;
    if( u<=32767 ) return 2;
    if( u<=8388607 ) return 3;
    if( u<=2147483647 ) return 4;
    if( u<=MAX_6BYTE ) return 5;
    return 6;
  }
  if( flags&MEM_Real ){
    return 7;
  }
  if( flags&MEM_Str ){
    int n = pMem->n;
    assert( n>=0 );
    return ((n*2) + 13);
  }
  if( flags&MEM_Blob ){
    return (pMem->n*2 + 12);
  }
  return 0;
}

/*
** Return the length of the data corresponding to the supplied serial-type.
*/
int sqlite3VdbeSerialTypeLen(u32 serial_type){
  if( serial_type>=12 ){
    return (serial_type-12)/2;
  }else{
    static const u8 aSize[] = { 0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0 };
    return aSize[serial_type];
  }
}

/*
** Write the serialized data blob for the value stored in pMem into 
** buf. It is assumed that the caller has allocated sufficient space.
** Return the number of bytes written.
*/ 
int sqlite3VdbeSerialPut(unsigned char *buf, Mem *pMem){
  u32 serial_type = sqlite3VdbeSerialType(pMem);
  int len;

  /* NULL */
  if( serial_type==0 ){
    return 0;
  }
 
  /* Integer and Real */
  if( serial_type<=7 ){
    u64 v;
    int i;
    if( serial_type==7 ){
      v = *(u64*)&pMem->r;
    }else{
      v = *(u64*)&pMem->i;
    }
    len = i = sqlite3VdbeSerialTypeLen(serial_type);
    while( i-- ){
      buf[i] = (v&0xFF);
      v >>= 8;
    }
    return len;
  }
  
  /* String or blob */
  assert( serial_type>=12 );
  len = sqlite3VdbeSerialTypeLen(serial_type);
  memcpy(buf, pMem->z, len);
  return len;
}

/*
** Deserialize the data blob pointed to by buf as serial type serial_type
** and store the result in pMem.  Return the number of bytes read.
*/ 
int sqlite3VdbeSerialGet(
  const unsigned char *buf,     /* Buffer to deserialize from */
  u32 serial_type,              /* Serial type to deserialize */
  Mem *pMem                     /* Memory cell to write value into */
){
  switch( serial_type ){
    case 8:    /* Reserved for future use */
    case 9:    /* Reserved for future use */
    case 10:   /* Reserved for future use */
    case 11:   /* Reserved for future use */
    case 0: {  /* NULL */
      pMem->flags = MEM_Null;
      break;
    }
    case 1: { /* 1-byte signed integer */
      pMem->i = (signed char)buf[0];
      pMem->flags = MEM_Int;
      return 1;
    }
    case 2: { /* 2-byte signed integer */
      pMem->i = (((signed char)buf[0])<<8) | buf[1];
      pMem->flags = MEM_Int;
      return 2;
    }
    case 3: { /* 3-byte signed integer */
      pMem->i = (((signed char)buf[0])<<16) | (buf[1]<<8) | buf[2];
      pMem->flags = MEM_Int;
      return 3;
    }
    case 4: { /* 4-byte signed integer */
      pMem->i = (buf[0]<<24) | (buf[1]<<16) | (buf[2]<<8) | buf[3];
      pMem->flags = MEM_Int;
      return 4;
    }
    case 5: { /* 6-byte signed integer */
      u64 x = (((signed char)buf[0])<<8) | buf[1];
      u32 y = (buf[2]<<24) | (buf[3]<<16) | (buf[4]<<8) | buf[5];
      x = (x<<32) | y;
      pMem->i = *(i64*)&x;
      pMem->flags = MEM_Int;
      return 6;
    }
    case 6:   /* 8-byte signed integer */
    case 7: { /* IEEE floating point */
      u64 x = (buf[0]<<24) | (buf[1]<<16) | (buf[2]<<8) | buf[3];
      u32 y = (buf[4]<<24) | (buf[5]<<16) | (buf[6]<<8) | buf[7];
#ifndef NDEBUG
      /* Verify that integers and floating point values use the same
      ** byte order.  The byte order differs on some (broken) architectures. */
      static const u64 t1 = ((u64)0x3ff00000)<<32;
      assert( 1.0==*(double*)&t1 );
#endif
      x = (x<<32) | y;
      if( serial_type==6 ){
        pMem->i = *(i64*)&x;
        pMem->flags = MEM_Int;
      }else{
        pMem->r = *(double*)&x;
        pMem->flags = MEM_Real;
      }
      return 8;
    }
    default: {
      int len = (serial_type-12)/2;
      pMem->z = (char *)buf;
      pMem->n = len;
      pMem->xDel = 0;
      if( serial_type&0x01 ){
        pMem->flags = MEM_Str | MEM_Ephem;
      }else{
        pMem->flags = MEM_Blob | MEM_Ephem;
      }
      return len;
    }
  }
  return 0;
}

/*
** This function compares the two table rows or index records specified by 
** {nKey1, pKey1} and {nKey2, pKey2}, returning a negative, zero
** or positive integer if {nKey1, pKey1} is less than, equal to or 
** greater than {nKey2, pKey2}.  Both Key1 and Key2 must be byte strings
** composed by the OP_MakeRecord opcode of the VDBE.
*/
int sqlite3VdbeRecordCompare(
  void *userData,
  int nKey1, const void *pKey1, 
  int nKey2, const void *pKey2
){
  KeyInfo *pKeyInfo = (KeyInfo*)userData;
  u32 d1, d2;          /* Offset into aKey[] of next data element */
  u32 idx1, idx2;      /* Offset into aKey[] of next header element */
  u32 szHdr1, szHdr2;  /* Number of bytes in header */
  int i = 0;
  int nField;
  int rc = 0;
  const unsigned char *aKey1 = (const unsigned char *)pKey1;
  const unsigned char *aKey2 = (const unsigned char *)pKey2;

  Mem mem1;
  Mem mem2;
  mem1.enc = pKeyInfo->enc;
  mem2.enc = pKeyInfo->enc;
  
  idx1 = sqlite3GetVarint32(pKey1, &szHdr1);
  d1 = szHdr1;
  idx2 = sqlite3GetVarint32(pKey2, &szHdr2);
  d2 = szHdr2;
  nField = pKeyInfo->nField;
  while( idx1<szHdr1 && idx2<szHdr2 ){
    u32 serial_type1;
    u32 serial_type2;

    /* Read the serial types for the next element in each key. */
    idx1 += sqlite3GetVarint32(&aKey1[idx1], &serial_type1);
    if( d1>=nKey1 && sqlite3VdbeSerialTypeLen(serial_type1)>0 ) break;
    idx2 += sqlite3GetVarint32(&aKey2[idx2], &serial_type2);
    if( d2>=nKey2 && sqlite3VdbeSerialTypeLen(serial_type2)>0 ) break;

    /* Assert that there is enough space left in each key for the blob of
    ** data to go with the serial type just read. This assert may fail if
    ** the file is corrupted.  Then read the value from each key into mem1
    ** and mem2 respectively.
    */
    d1 += sqlite3VdbeSerialGet(&aKey1[d1], serial_type1, &mem1);
    d2 += sqlite3VdbeSerialGet(&aKey2[d2], serial_type2, &mem2);

    rc = sqlite3MemCompare(&mem1, &mem2, i<nField ? pKeyInfo->aColl[i] : 0);
    if( mem1.flags & MEM_Dyn ) sqlite3VdbeMemRelease(&mem1);
    if( mem2.flags & MEM_Dyn ) sqlite3VdbeMemRelease(&mem2);
    if( rc!=0 ){
      break;
    }
    i++;
  }

  /* One of the keys ran out of fields, but all the fields up to that point
  ** were equal. If the incrKey flag is true, then the second key is
  ** treated as larger.
  */
  if( rc==0 ){
    if( pKeyInfo->incrKey ){
      rc = -1;
    }else if( d1<nKey1 ){
      rc = 1;
    }else if( d2<nKey2 ){
      rc = -1;
    }
  }

  if( pKeyInfo->aSortOrder && i<pKeyInfo->nField && pKeyInfo->aSortOrder[i] ){
    rc = -rc;
  }

  return rc;
}

/*
** The argument is an index entry composed using the OP_MakeRecord opcode.
** The last entry in this record should be an integer (specifically
** an integer rowid).  This routine returns the number of bytes in
** that integer.
*/
int sqlite3VdbeIdxRowidLen(int nKey, const u8 *aKey){
  u32 szHdr;        /* Size of the header */
  u32 typeRowid;    /* Serial type of the rowid */

  sqlite3GetVarint32(aKey, &szHdr);
  sqlite3GetVarint32(&aKey[szHdr-1], &typeRowid);
  return sqlite3VdbeSerialTypeLen(typeRowid);
}
  

/*
** pCur points at an index entry created using the OP_MakeRecord opcode.
** Read the rowid (the last field in the record) and store it in *rowid.
** Return SQLITE_OK if everything works, or an error code otherwise.
*/
int sqlite3VdbeIdxRowid(BtCursor *pCur, i64 *rowid){
  i64 nCellKey;
  int rc;
  u32 szHdr;        /* Size of the header */
  u32 typeRowid;    /* Serial type of the rowid */
  u32 lenRowid;     /* Size of the rowid */
  Mem m, v;

  sqlite3BtreeKeySize(pCur, &nCellKey);
  if( nCellKey<=0 ){
    return SQLITE_CORRUPT;
  }
  rc = sqlite3VdbeMemFromBtree(pCur, 0, nCellKey, 1, &m);
  if( rc ){
    return rc;
  }
  sqlite3GetVarint32(m.z, &szHdr);
  sqlite3GetVarint32(&m.z[szHdr-1], &typeRowid);
  lenRowid = sqlite3VdbeSerialTypeLen(typeRowid);
  sqlite3VdbeSerialGet(&m.z[m.n-lenRowid], typeRowid, &v);
  *rowid = v.i;
  sqlite3VdbeMemRelease(&m);
  return SQLITE_OK;
}

/*
** Compare the key of the index entry that cursor pC is point to against
** the key string in pKey (of length nKey).  Write into *pRes a number
** that is negative, zero, or positive if pC is less than, equal to,
** or greater than pKey.  Return SQLITE_OK on success.
**
** pKey is either created without a rowid or is truncated so that it
** omits the rowid at the end.  The rowid at the end of the index entry
** is ignored as well.
*/
int sqlite3VdbeIdxKeyCompare(
  Cursor *pC,                 /* The cursor to compare against */
  int nKey, const u8 *pKey,   /* The key to compare */
  int *res                    /* Write the comparison result here */
){
  i64 nCellKey;
  int rc;
  BtCursor *pCur = pC->pCursor;
  int lenRowid;
  Mem m;

  sqlite3BtreeKeySize(pCur, &nCellKey);
  if( nCellKey<=0 ){
    *res = 0;
    return SQLITE_OK;
  }
  rc = sqlite3VdbeMemFromBtree(pC->pCursor, 0, nCellKey, 1, &m);
  if( rc ){
    return rc;
  }
  lenRowid = sqlite3VdbeIdxRowidLen(m.n, m.z);
  *res = sqlite3VdbeRecordCompare(pC->pKeyInfo, m.n-lenRowid, m.z, nKey, pKey);
  sqlite3VdbeMemRelease(&m);
  return SQLITE_OK;
}

/*
** This routine sets the value to be returned by subsequent calls to
** sqlite3_changes() on the database handle 'db'. 
*/
void sqlite3VdbeSetChanges(sqlite3 *db, int nChange){
  db->nChange = nChange;
  db->nTotalChange += nChange;
}

/*
** Set a flag in the vdbe to update the change counter when it is finalised
** or reset.
*/
void sqlite3VdbeCountChanges(Vdbe *v){
  v->changeCntOn = 1;
}

/*
** Mark every prepared statement associated with a database connection
** as expired.
**
** An expired statement means that recompilation of the statement is
** recommend.  Statements expire when things happen that make their
** programs obsolete.  Removing user-defined functions or collating
** sequences, or changing an authorization function are the types of
** things that make prepared statements obsolete.
*/
void sqlite3ExpirePreparedStatements(sqlite3 *db){
  Vdbe *p;
  for(p = db->pVdbe; p; p=p->pNext){
    p->expired = 1;
  }
}

/*
** Return the database associated with the Vdbe.
*/
sqlite3 *sqlite3VdbeDb(Vdbe *v){
  return v->db;
}
