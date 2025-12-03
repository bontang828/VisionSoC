// Trap never delegate to S Mode when Supervisor is not supported
func TryDelegateTrap(cause : integer{0..63}, trap_value : bits(32)) => boolean
begin
  return FALSE;
end

func TrapSupervisorInterrupt(icode : integer) => boolean
begin
  // for impl without supervisor, all interrupt are not handled
  return FALSE;
end

func InterruptDelegatable(i : integer) => boolean
begin
  // for impl without supervisor, all interrupt can't be delegated
  return FALSE;
end

func CheckSupervisorInterrupt() => integer{0}
begin
  // for impl without supervisor, all pending bits and external signals are not exists
  return 0;
end
