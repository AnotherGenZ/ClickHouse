#include <Interpreters/HashJoin/HashJoinMethodsImpl.h>

namespace DB
{
template class HashJoinMethods<JoinKind::Left, JoinStrictness::All, HashJoin::MapsAll>;
}
