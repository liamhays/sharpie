# TLV758 uses the equation
#     V_out = V_FB * (1 + R1/R2)
# where V_FB = 0.55 V, to determine the output voltage based on a
# resistor divider between OUT and FB. What ,
# because we can, so this script finds appropriate values for those
# two resistors via iteration.

r_values = (10, 11, 12, 13, 15, 16, 18, 20, 22, 24, 27, 30, 33,
            36, 39, 43, 47, 51, 56, 62, 68, 75, 82, 91)

# r_values = (10, 10.2, 10.5, 10.7, 11, 11.3, 11.5, 11.8, 12.1, 12.4,
#             12.7, 13.0, 13.3, 13.7, 14.0, 14.3, 14.7, 15.0, 15.4,
#             15.8, 16.2, 16.5, 16.9, 17.4, 17.8, 18.2, 18.7, 19.1,
#             19.6, 20, 20.5, 21, 21.5, 22.1, 22.6, 23.2, 23.7, 24.3,
#             24.9, 25.5, 26.1, 26.7, 27.4, 28, 28.7, 29.4, 30.1,
#             30.9, 31.6, 32.4, 33.2, 34, 34.8, 35.7, 36.5, 37.4,
#             38.3, 39.2, 40.2, 41.2, 42.2, 43.2, 44.2, 45.3,
#             46.4, 47.5, 48.7, 49.9, 51.1, 52.3, 53.6, 54.9, 56.2,
#             57.6, 59, 60.4, 61.9, 63.4, 64.9, 66.5, 68.1, 69.8,
#             71.5, 73.2, 75, 76.8, 78.7, 80.6, 82.5, 84.5, 86.6,
#             88.7, 90.9, 93.1, 95.3, 97.6)

def vout(r1: int, r2: int) -> float:
    v = 0.55*(1 + r1/r2)
    return v

# for each value, pair it with another value and check
# the output voltage
def r_finder(r_values: tuple):
    for r1 in r_values:
        for r2 in r_values:
            v = vout(r1, r2)
            if v > (3.2 - 0.05) and v < (3.2 + 0.05):
                print(f'r1: {r1} / r2: {r2} = vout: {v}')



if __name__ == '__main__':
    # a forum post recommends values between 10k and 100k

    # resistor values scale linearly by 10, so adjust the
    # calculated resistances accordingly
    r_finder(r_values)
