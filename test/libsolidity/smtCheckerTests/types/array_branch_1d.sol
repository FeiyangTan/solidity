pragma experimental SMTChecker;

contract C
{
	function f(bool b, uint[] memory c) public pure {
		require(c.length >= 1 && c.length <= 2);
		c[0] = 0;
		if (b)
			c[0] = 1;
		assert(c[0] > 0);
	}
}
// ====
// SMTIgnoreCex: yes
// ----
// Warning 6328: (176-192): CHC: Assertion violation happens here.
