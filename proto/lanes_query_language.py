from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Set, Tuple, Union
import re


# ============================================================
# Tiny query language V2
# ============================================================
#
# Model:
#   KeySegment = /[a-z_]+/
#   Key        = KeySegment ('.' KeySegment)*
#   Atom       = unit | int | float
#   Element    = Dict[Key, Atom]
#
# Main expression classes:
#   ElementSetExpr: returns matching element indexes.
#   ValueSetExpr:   matches / produces values.
#
# Important syntax:
#   key                      key exists, exact full-key only
#   key=value_expr           key exists and value matches value_expr
#   !expr                    negation
#   a b                      element AND
#   a | b                    element OR
#   tag.(red blue | green)   expands to (tag.red tag.blue) | tag.green
#   a.b.(c d | e).f          expands to (a.b.c.f a.b.d.f) | a.b.e.f
#   parent=filter.selected:id
#                            parent equals any id value from selected elements
#
# Missing projection values are skipped. No null/undefined exists.


# -----------------------------
# Data model
# -----------------------------

class _Unit:
    def __repr__(self) -> str:
        return "unit"

    def __str__(self) -> str:
        return "unit"


unit = _Unit()
Atom = Union[int, float, _Unit]
Element = Dict[str, Atom]
ElementId = int


# -----------------------------
# Tokenizer
# -----------------------------

Token = Tuple[str, str]

TOKEN_RE = re.compile(
    r"""
    (?P<float>-?[0-9]+\.[0-9]+)
  | (?P<int>-?[0-9]+)
  | (?P<range>\.\.)
  | (?P<ident>[a-z_]+)
  | (?P<op>[().:=|!])
  | (?P<ws>\s+)
  | (?P<bad>.)
    """,
    re.VERBOSE,
)

RESERVED_VALUE_WORDS = {"unit"}


def tokenize(src: str) -> List[Token]:
    tokens: List[Token] = []
    for m in TOKEN_RE.finditer(src):
        kind = m.lastgroup or ""
        text = m.group()
        if kind == "ws":
            continue
        if kind == "bad":
            raise SyntaxError(f"unexpected character {text!r}")
        tokens.append((kind, text))
    tokens.append(("eof", ""))
    return tokens


# -----------------------------
# AST
# -----------------------------

@dataclass(frozen=True)
class Key:
    path: str


@dataclass(frozen=True)
class Exists:
    key: Key


@dataclass(frozen=True)
class HasValue:
    key: Key
    value: "ValueExpr"


@dataclass(frozen=True)
class Not:
    expr: "ElementExpr"


@dataclass(frozen=True)
class And:
    parts: Tuple["ElementExpr", ...]


@dataclass(frozen=True)
class Or:
    parts: Tuple["ElementExpr", ...]


ElementExpr = Union[Exists, HasValue, Not, And, Or]


@dataclass(frozen=True)
class Literal:
    value: Atom


@dataclass(frozen=True)
class Range:
    lo: Optional[float]
    hi: Optional[float]


@dataclass(frozen=True)
class VNot:
    expr: "ValueExpr"


@dataclass(frozen=True)
class VOr:
    parts: Tuple["ValueExpr", ...]


@dataclass(frozen=True)
class Projection:
    source: ElementExpr
    selector: Key


ValueExpr = Union[Literal, Range, VNot, VOr, Projection]


@dataclass(frozen=True)
class IdentComp:
    name: str


@dataclass(frozen=True)
class GroupComp:
    expr: ElementExpr


PathComp = Union[IdentComp, GroupComp]


# -----------------------------
# Parser
# -----------------------------

class Parser:
    def __init__(self, tokens: Sequence[Token]):
        self.tokens = list(tokens)
        self.i = 0

    def peek(self) -> Token:
        return self.tokens[self.i]

    def peek_text(self) -> str:
        return self.peek()[1]

    def pop(self, expected: Optional[str] = None) -> Token:
        tok = self.peek()
        if expected is not None and tok[1] != expected:
            raise SyntaxError(f"expected {expected!r}, got {tok[1]!r}")
        self.i += 1
        return tok

    def accept(self, text: str) -> bool:
        if self.peek_text() == text:
            self.i += 1
            return True
        return False

    def parse(self) -> ElementExpr:
        expr = self.parse_element_or(stop={""})
        if self.peek()[0] != "eof":
            raise SyntaxError(f"trailing token {self.peek_text()!r}")
        return expr

    # -------------------------
    # Element expressions
    # -------------------------

    def parse_element_or(self, stop: Set[str]) -> ElementExpr:
        parts = [self.parse_element_and(stop | {"|"})]
        while self.peek_text() == "|" and "|" not in stop:
            self.pop("|")
            parts.append(self.parse_element_and(stop | {"|"}))
        return parts[0] if len(parts) == 1 else Or(tuple(parts))

    def parse_element_and(self, stop: Set[str]) -> ElementExpr:
        parts = [self.parse_element_unary(stop)]
        while self.peek_text() not in stop and self.peek()[0] != "eof":
            parts.append(self.parse_element_unary(stop))
        return parts[0] if len(parts) == 1 else And(tuple(parts))

    def parse_element_unary(self, stop: Set[str]) -> ElementExpr:
        if self.accept("!"):
            return Not(self.parse_element_unary(stop))
        return self.parse_element_primary(stop)

    def parse_element_primary(self, stop: Set[str]) -> ElementExpr:
        # Parenthesized element group, or leading grouped path context: (a | b).c
        if self.accept("("):
            inner = self.parse_element_or(stop={")"})
            self.pop(")")

            # (a | b).c form: treat the group as a path component.
            if self.peek_text() == ".":
                comps: List[PathComp] = [GroupComp(inner)]
                while self.accept("."):
                    comps.append(self.parse_path_component())
                expr = compile_path_components(comps)
                if self.accept("="):
                    expr = element_keys_to_value_tests(expr, self.parse_value_or(stop=stop))
                return expr

            # Optional: (a | b)=3 means a=3 | b=3.
            if self.accept("="):
                return element_keys_to_value_tests(inner, self.parse_value_or(stop=stop))

            return inner

        comps = self.parse_path_components()
        expr = compile_path_components(comps)

        if self.accept("="):
            value_expr = self.parse_value_or(stop=stop)
            expr = element_keys_to_value_tests(expr, value_expr)

        return expr

    # Parses a path/context expression, such as:
    #   a.b
    #   a.(b c | d).e
    #   (only called after we know we are not parsing a plain parenthesized group)
    def parse_path_components(self) -> List[PathComp]:
        comps = [self.parse_path_component()]
        while self.accept("."):
            comps.append(self.parse_path_component())
        return comps

    def parse_path_component(self) -> PathComp:
        if self.accept("("):
            inner = self.parse_element_or(stop={")"})
            self.pop(")")
            return GroupComp(inner)

        tok = self.pop()
        if tok[0] != "ident" or tok[1] in RESERVED_VALUE_WORDS:
            raise SyntaxError(f"expected key segment, got {tok[1]!r}")
        return IdentComp(tok[1])

    # -------------------------
    # Value expressions
    # -------------------------

    def parse_value_or(self, stop: Set[str]) -> ValueExpr:
        # In value context, | belongs to values until the caller's stop closes it.
        parts = [self.parse_value_unary(stop | {"|"})]
        while self.peek_text() == "|" and "|" not in stop:
            self.pop("|")
            parts.append(self.parse_value_unary(stop | {"|"}))
        return parts[0] if len(parts) == 1 else VOr(tuple(parts))

    def parse_value_unary(self, stop: Set[str]) -> ValueExpr:
        if self.accept("!"):
            return VNot(self.parse_value_unary(stop))
        return self.parse_value_primary(stop)

    def parse_value_primary(self, stop: Set[str]) -> ValueExpr:
        if self.accept("("):
            # Ambiguity: (element query):selector is a projection source;
            # otherwise parentheses group a value expression.
            saved = self.i
            try:
                source = self.parse_element_or(stop={")"})
                self.pop(")")
                if self.accept(":"):
                    selector = self.parse_concrete_key()
                    return Projection(source, selector)
            except SyntaxError:
                pass
            self.i = saved
            value = self.parse_value_or(stop={")"})
            self.pop(")")
            return value

        # Range with missing lower bound: ..N
        if self.accept(".."):
            return Range(None, self.parse_number())

        # unit
        if self.peek_text() == "unit":
            self.pop()
            return Literal(unit)

        # Number or range starting with a number: N, N.., N..M
        if self.peek()[0] in {"int", "float"}:
            number_text = self.peek()[1]
            n = self.parse_number()
            if self.accept(".."):
                hi = self.parse_number() if self.peek()[0] in {"int", "float"} else None
                return Range(float(n), None if hi is None else float(hi))
            return Literal(parse_number_atom(number_text))

        # Projection with non-parenthesized source: source:selector
        if self.peek()[0] == "ident":
            source_comps = self.parse_path_components()
            source = compile_path_components(source_comps)
            if not self.accept(":"):
                raise SyntaxError("value expression key source must be followed by ':' selector")
            selector = self.parse_concrete_key()
            return Projection(source, selector)

        raise SyntaxError(f"expected value expression, got {self.peek_text()!r}")

    def parse_number(self) -> float:
        tok = self.pop()
        if tok[0] == "int":
            return float(int(tok[1]))
        if tok[0] == "float":
            return float(tok[1])
        raise SyntaxError(f"expected number, got {tok[1]!r}")

    def parse_concrete_key(self) -> Key:
        parts: List[str] = []
        tok = self.pop()
        if tok[0] != "ident" or tok[1] in RESERVED_VALUE_WORDS:
            raise SyntaxError(f"expected selector key segment, got {tok[1]!r}")
        parts.append(tok[1])
        while self.accept("."):
            tok = self.pop()
            if tok[0] != "ident" or tok[1] in RESERVED_VALUE_WORDS:
                raise SyntaxError(f"expected selector key segment, got {tok[1]!r}")
            parts.append(tok[1])
        return Key(".".join(parts))


# -----------------------------
# AST rewriting for key contexts
# -----------------------------

def parse_key_to_components(key: Key) -> List[PathComp]:
    return [IdentComp(p) for p in key.path.split(".")]


def compile_path_components(comps: Sequence[PathComp]) -> ElementExpr:
    """Compile a path/context expression into an ElementExpr.

    Examples:
      a.b                 -> Exists(a.b)
      a.(b | c).d         -> Exists(a.b.d) | Exists(a.c.d)
      a.b.(c d | e).f     -> (Exists(a.b.c.f) Exists(a.b.d.f)) | Exists(a.b.e.f)
    """
    group_index = next((i for i, c in enumerate(comps) if isinstance(c, GroupComp)), None)

    if group_index is None:
        names = [c.name for c in comps if isinstance(c, IdentComp)]
        return Exists(Key(".".join(names)))

    prefix = [c.name for c in comps[:group_index] if isinstance(c, IdentComp)]
    group = comps[group_index]
    assert isinstance(group, GroupComp)
    suffix = list(comps[group_index + 1 :])
    return apply_context(group.expr, prefix=prefix, suffix=suffix)


def compile_key_with_context(key: Key, prefix: Sequence[str], suffix: Sequence[PathComp]) -> ElementExpr:
    return compile_path_components([IdentComp(p) for p in prefix] + parse_key_to_components(key) + list(suffix))


def apply_context(expr: ElementExpr, prefix: Sequence[str], suffix: Sequence[PathComp]) -> ElementExpr:
    """Apply key prefix/suffix context to every key occurrence inside expr."""
    if isinstance(expr, Exists):
        return compile_key_with_context(expr.key, prefix, suffix)

    if isinstance(expr, HasValue):
        base = compile_key_with_context(expr.key, prefix, suffix)
        return element_keys_to_value_tests(base, expr.value)

    if isinstance(expr, Not):
        return Not(apply_context(expr.expr, prefix, suffix))

    if isinstance(expr, And):
        return And(tuple(apply_context(p, prefix, suffix) for p in expr.parts))

    if isinstance(expr, Or):
        return Or(tuple(apply_context(p, prefix, suffix) for p in expr.parts))

    raise TypeError(expr)


def element_keys_to_value_tests(expr: ElementExpr, value: ValueExpr) -> ElementExpr:
    """Convert every key-presence predicate in expr into key=value.

    This supports:
      a.(b | c).d=3   -> a.b.d=3 | a.c.d=3
      a.(b c).d=3     -> a.b.d=3 a.c.d=3
    """
    if isinstance(expr, Exists):
        return HasValue(expr.key, value)

    if isinstance(expr, HasValue):
        # Already a value test. Replacing it would be surprising, so reject.
        raise SyntaxError("cannot apply '= value' to an expression that already contains value tests")

    if isinstance(expr, Not):
        return Not(element_keys_to_value_tests(expr.expr, value))

    if isinstance(expr, And):
        return And(tuple(element_keys_to_value_tests(p, value) for p in expr.parts))

    if isinstance(expr, Or):
        return Or(tuple(element_keys_to_value_tests(p, value) for p in expr.parts))

    raise TypeError(expr)


def parse_number_atom(text: str) -> Union[int, float]:
    return float(text) if "." in text else int(text)


# -----------------------------
# Evaluator
# -----------------------------

class Engine:
    def __init__(self, elements: Sequence[Element]):
        self.elements = list(elements)
        self.all_ids: Set[ElementId] = set(range(len(self.elements)))

    def parse(self, query: str) -> ElementExpr:
        return Parser(tokenize(query)).parse()

    def eval(self, query: str) -> Set[ElementId]:
        return self.eval_element(self.parse(query))

    def eval_element(self, expr: ElementExpr) -> Set[ElementId]:
        if isinstance(expr, Exists):
            return {i for i, e in enumerate(self.elements) if expr.key.path in e}

        if isinstance(expr, HasValue):
            out: Set[ElementId] = set()
            for i, e in enumerate(self.elements):
                if expr.key.path in e and self.value_matches(e[expr.key.path], expr.value):
                    out.add(i)
            return out

        if isinstance(expr, Not):
            return self.all_ids - self.eval_element(expr.expr)

        if isinstance(expr, And):
            result = set(self.all_ids)
            for part in expr.parts:
                result &= self.eval_element(part)
            return result

        if isinstance(expr, Or):
            result: Set[ElementId] = set()
            for part in expr.parts:
                result |= self.eval_element(part)
            return result

        raise TypeError(expr)

    def finite_value_set(self, expr: ValueExpr) -> Set[Atom]:
        if isinstance(expr, Literal):
            return {expr.value}

        if isinstance(expr, Projection):
            source_ids = self.eval_element(expr.source)
            return {
                self.elements[i][expr.selector.path]
                for i in source_ids
                if expr.selector.path in self.elements[i]
            }

        if isinstance(expr, VOr):
            out: Set[Atom] = set()
            for p in expr.parts:
                out |= self.finite_value_set(p)
            return out

        raise TypeError("value expression is not a finite value set")

    def value_matches(self, value: Atom, expr: ValueExpr) -> bool:
        if isinstance(expr, Literal):
            return value == expr.value

        if isinstance(expr, Range):
            if value is unit:
                return False
            v = float(value)
            if expr.lo is not None and v < expr.lo:
                return False
            if expr.hi is not None and v > expr.hi:
                return False
            return True

        if isinstance(expr, VNot):
            return not self.value_matches(value, expr.expr)

        if isinstance(expr, VOr):
            return any(self.value_matches(value, p) for p in expr.parts)

        if isinstance(expr, Projection):
            return value in self.finite_value_set(expr)

        raise TypeError(expr)


# -----------------------------
# Tiny smoke test / demo
# -----------------------------

if __name__ == "__main__":
    elements: List[Element] = [
        {"id": 1, "filter.selected": unit, "type.folder": unit, "tag.red": unit},
        {"id": 2, "parent": 1, "tag.red": unit, "price": 5, "a.b.c.f": unit, "a.b.d.f": unit},
        {"id": 3, "parent": 1, "tag.blue": unit, "price": 12, "a.b.e.f": unit},
        {"id": 4, "parent": 99, "tag.green": unit, "price": -3, "a.c.d": 3},
        {"id": 5, "inventory": -3, "a.b.d": 4},
        {"id": 6, "inventory": 2, "a.c.d": 4},
        {"id": 7, "a.c.f": unit},
        {"id": 8, "b.c.d": unit},
    ]

    engine = Engine(elements)

    queries = [
        "filter.selected",
        "tag.(red | blue)",
        "tag.(red blue | green)",
        "price=..10",
        "price=!(..10)",
        "inventory=!-3",
        "!inventory=-3",
        "parent=filter.selected:id",
        "parent=(filter.selected type.folder):id",
        "a.b.(c d | e).f",
        "a.(b | c).d=(3 | 4)",
        "(a | b).c.(d | f)",
    ]

    for q in queries:
        indexes = sorted(engine.eval(q))
        public_ids = [elements[i].get("id") for i in indexes]
        print(f"{q:42} -> indexes {indexes}, ids {public_ids}")
